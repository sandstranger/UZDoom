/*
** gl_shader.cpp
**
** GLSL shader handling
**
**---------------------------------------------------------------------------
**
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "gl_system.h"
#include "c_cvars.h"
#include "v_video.h"
#include "filesystem.h"
#include "engineerrors.h"
#include "cmdlib.h"
#include "md5.h"
#include "gl_shader.h"
#include "hw_shaderpatcher.h"
#include "shaderuniforms.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "i_specialpaths.h"
#include "printf.h"
#include "version.h"
#include "stb_include.h"

#include "gl_interface.h"
#include "gl_debug.h"
#include "matrix.h"
#include "gl_renderer.h"
#include "hw_programcache.h"
#include <map>
#include <memory>
#if ANDROID
#include "SDL.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#endif

EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Int, gl_ssao)
#ifdef ANDROID
EXTERN_CVAR(Bool, gl_customshader)
#include <atomic>
extern std::atomic<bool> gl_lite_shader;
// (atomic: the launcher sets it through JNA from its own thread)
std::atomic<bool> gEnableSpirvCross{false};

extern "C" {
__attribute__((used)) __attribute__((visibility("default")))
void setSpirvCrossState(const int enableSpirvCross) {
    gEnableSpirvCross = enableSpirvCross != 0;
}
}
#endif

namespace OpenGLRenderer
{

#if ANDROID
    // Desktop GLSL fed to NG-GL4ES (SPIRV-Cross) and the GLSL ES version it must emit (part of
    // the program cache key). The Android GL renderer is a GLES 3.0 renderer: GLSL ES 3.00 only.
    constexpr int NGGL4ES_SOURCE_GLSL = 410;
    constexpr int NGGL4ES_TARGET_ESSL = 300;
    int NGGL4ESTargetESSL()
    {
        return NGGL4ES_TARGET_ESSL;
    }

    typedef char* (*GLSLtoGLSLES_t)(const char*, GLenum, unsigned int, unsigned int, int*);
	static void* ngGL4ESPTR = nullptr;
	static GLSLtoGLSLES_t GLSLtoGLSLES_c = nullptr;

	// On the render thread, before any worker can run a conversion.
	static void LoadConverter()
	{
		if (GLSLtoGLSLES_c == nullptr)
		{
			if (ngGL4ESPTR == nullptr) ngGL4ESPTR = SDL_LoadObject("libng_gl4es.so");
			if (ngGL4ESPTR != nullptr) GLSLtoGLSLES_c = (GLSLtoGLSLES_t)SDL_LoadFunction(ngGL4ESPTR, "GLSLtoGLSLES_c");
			if (GLSLtoGLSLES_c == nullptr) I_FatalError("Unable to load the GLSL converter (libng_gl4es.so): %s\n", SDL_GetError());
		}
	}

	// The GLES renderer's post-process shaders: the converter if it can be loaded, else they are
	// patched to GLSL ES directly (no fatal error).
	bool TryLoadShaderConverter()
	{
		if (GLSLtoGLSLES_c == nullptr)
		{
			if (ngGL4ESPTR == nullptr) ngGL4ESPTR = SDL_LoadObject("libng_gl4es.so");
			if (ngGL4ESPTR != nullptr) GLSLtoGLSLES_c = (GLSLtoGLSLES_t)SDL_LoadFunction(ngGL4ESPTR, "GLSLtoGLSLES_c");
		}
		return GLSLtoGLSLES_c != nullptr;
	}

	// GLSL -> SPIR-V -> GLSL ES: CPU work only, no GL calls. NG-GL4ES's converter is reentrant
	// (glslang with per-thread pools, a SPIRV-Tools optimizer per thread, a SPIRV-Cross context
	// per call).
	static std::string RunConverter(const char *source, bool isVertexShader)
	{
		int returnCode = 0;
		char *glesShader = GLSLtoGLSLES_c(source, isVertexShader ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER,
			NGGL4ES_TARGET_ESSL, NGGL4ES_SOURCE_GLSL, &returnCode);
		if (glesShader == nullptr) return source;	// the driver reports what is wrong with it
		std::string result = glesShader;
		free(glesShader);
		return result;
	}

	//==========================================================================
	//
	// Conversions done ahead on worker threads. When a shader collection starts
	// compiling, the sources of all its programs that are not in the binary
	// cache are queued (FShaderCollection::PrefetchSources); the render thread
	// then only compiles and links, and takes queued conversions itself while it
	// waits for one. The conversion was nearly all of the time the startup
	// spent building programs (about 0.4 s per program on a phone CPU), with
	// the start screen frozen meanwhile.
	//
	// The workers exit when the queue is empty and are joined when the
	// collection is done: the converter's per-thread memory (glslang pools, the
	// SPIR-V optimizer) does not stay around.
	//
	//==========================================================================

	struct ConversionJob
	{
		std::string source;
		std::string result;
		bool vertex = false;
		bool done = false;
		int uses = 0;		// programs that will ask for this conversion
	};

	static std::mutex ConvLock;
	static std::condition_variable ConvDone;
	static std::deque<std::shared_ptr<ConversionJob>> ConvQueue;
	static std::unordered_multimap<size_t, std::shared_ptr<ConversionJob>> ConvJobs;
	static std::vector<std::thread> ConvWorkers;
	static int ConvRunning = 0;

	static size_t ConvHash(const char *source, bool vertex)
	{
		return std::hash<std::string_view>()(std::string_view(source)) * 2 + (vertex ? 1 : 0);
	}

	// ConvLock held.
	static std::shared_ptr<ConversionJob> FindConversion(const char *source, bool vertex)
	{
		auto range = ConvJobs.equal_range(ConvHash(source, vertex));
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second->vertex == vertex && it->second->source == source) return it->second;
		}
		return nullptr;
	}

	// ConvLock held.
	static void ForgetConversion(const std::shared_ptr<ConversionJob> &job)
	{
		auto range = ConvJobs.equal_range(ConvHash(job->source.c_str(), job->vertex));
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second == job)
			{
				ConvJobs.erase(it);
				return;
			}
		}
	}

	// ConvLock held on entry and exit, released while converting.
	static void RunQueuedConversion(std::unique_lock<std::mutex> &lock)
	{
		auto job = ConvQueue.front();
		ConvQueue.pop_front();
		lock.unlock();
		std::string result = RunConverter(job->source.c_str(), job->vertex);
		lock.lock();
		job->result = std::move(result);
		job->done = true;
		ConvDone.notify_all();
	}

	static void ConversionWorker()
	{
		std::unique_lock<std::mutex> lock(ConvLock);
		while (!ConvQueue.empty()) RunQueuedConversion(lock);
		ConvRunning--;
	}

	static int MaxConversionWorkers()
	{
		// The render thread converts as well. A conversion of the main shader takes a few tens
		// of MB while it runs: fewer threads on 32-bit builds.
		const int cores = (int)std::thread::hardware_concurrency();
		return clamp(cores - 1, 1, sizeof(void*) < 8 ? 2 : 4);
	}

	static void QueueShaderConversion(const FString &source, bool isVertexShader)
	{
		LoadConverter();
		std::lock_guard<std::mutex> lock(ConvLock);
		if (auto job = FindConversion(source.GetChars(), isVertexShader))
		{
			job->uses++;	// same source in another program (vertex shaders often are)
			return;
		}
		auto job = std::make_shared<ConversionJob>();
		job->source = source.GetChars();
		job->vertex = isVertexShader;
		job->uses = 1;
		ConvJobs.emplace(ConvHash(source.GetChars(), isVertexShader), job);
		ConvQueue.push_back(job);
		if (ConvRunning < MaxConversionWorkers())
		{
			if (ConvRunning == 0)
			{
				// the earlier workers have all left their loop
				for (auto &worker : ConvWorkers) worker.join();
				ConvWorkers.clear();
			}
			ConvWorkers.emplace_back(ConversionWorker);
			ConvRunning++;
		}
	}

	// Joins the workers and drops what no program asked for (sources that came out different).
	static void FinishShaderConversions()
	{
		std::vector<std::thread> workers;
		{
			std::lock_guard<std::mutex> lock(ConvLock);
			ConvQueue.clear();
			workers.swap(ConvWorkers);
		}
		for (auto &worker : workers) worker.join();
		std::lock_guard<std::mutex> lock(ConvLock);
		ConvJobs.clear();
	}

    std::string ConvertShaderToGLES(const char* shaderSource, bool isVertexShader)
    {
		LoadConverter();
		{
			std::unique_lock<std::mutex> lock(ConvLock);
			if (auto job = FindConversion(shaderSource, isVertexShader))
			{
				// Not started yet: this one first. Then help with the queue instead of waiting idle.
				auto queued = std::find(ConvQueue.begin(), ConvQueue.end(), job);
				if (queued != ConvQueue.end() && queued != ConvQueue.begin())
				{
					ConvQueue.erase(queued);
					ConvQueue.push_front(job);
				}
				while (!job->done)
				{
					if (!ConvQueue.empty()) RunQueuedConversion(lock);
					else ConvDone.wait(lock);
				}
				if (--job->uses > 0) return job->result;
				std::string result = std::move(job->result);
				ForgetConversion(job);
				return result;
			}
		}
		return RunConverter(shaderSource, isVertexShader);
    }

	void UnloadNGGL4ESPTR()
	{
		FinishShaderConversions();
		if (ngGL4ESPTR!= nullptr)
		{
			SDL_UnloadObject(ngGL4ESPTR);
			ngGL4ESPTR = nullptr;
		}
		GLSLtoGLSLES_c = nullptr;
	}
#endif


bool IsShaderCacheActive()
{
#ifdef ANDROID
	// ES 3.0 core, but a driver may expose zero binary formats (then every binary is refused).
	static int numFormats = -1;
	if (numFormats < 0)
	{
		GLint n = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
		numFormats = n > 0 ? n : 0;
	}
	return numFormats > 0;
#endif
	static bool active = true;
	static bool firstcall = true;

	if (firstcall)
	{
		const char *vendor = (const char *)glGetString(GL_VENDOR);
		active = !(strstr(vendor, "Intel") == nullptr);
		firstcall = false;
	}
	return active;
}

// Program binaries live in the shared per-driver cache (hw_programcache.cpp). The namespace keeps
// binaries built from differently converted sources apart; the pipeline tag changes whenever the
// conversion of the sources changes without the sources themselves changing.
static const char *ProgramCacheNamespace()
{
#ifdef ANDROID
	return gEnableSpirvCross ? "gl-spirv" : "gl-essl";
#else
	return "gl";
#endif
}

static FString ProgramCacheDriver()
{
	auto str = [](GLenum e) { const char *s = (const char *)glGetString(e); return s ? s : ""; };
	FString driver;
	driver.Format("%s|%s|%s|%s", str(GL_VENDOR), str(GL_RENDERER), str(GL_VERSION), str(GL_SHADING_LANGUAGE_VERSION));
	return driver;
}

static FString ProgramCacheKey(const FString &vertex, const FString &fragment)
{
	FString pipeline = GetGitHash();
#ifdef ANDROID
	if (gEnableSpirvCross)
		pipeline.AppendFormat("|ngl4es essl%d from glsl%d", NGGL4ESTargetESSL(), NGGL4ES_SOURCE_GLSL);
#endif
	return ProgramBinaryCache::MakeKey(ProgramCacheNamespace(), ProgramCacheDriver().GetChars(), pipeline.GetChars(), vertex, fragment);
}

TArray<uint8_t> LoadCachedProgramBinary(const FString &vertex, const FString &fragment, uint32_t &binaryFormat)
{
	TArray<uint8_t> data;
	if (!ProgramBinaryCache::Load(ProgramCacheNamespace(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment), binaryFormat, data))
		binaryFormat = 0;
	return data;
}

void SaveCachedProgramBinary(const FString &vertex, const FString &fragment, const TArray<uint8_t> &binary, uint32_t binaryFormat)
{
	ProgramBinaryCache::Store(ProgramCacheNamespace(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment), binaryFormat, binary);
}

void RejectCachedProgramBinary(const FString &vertex, const FString &fragment)
{
	ProgramBinaryCache::Reject(ProgramCacheNamespace(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment));
}

FString ProcessShaderError(const char * shaderError, TArray<FString> &filenames_for_error)
{
	//ugh, intel, amd and nvidia handle things differently so this has to be a mess
	enum
	{
		READING_LUMP,
		READING_LINE_COLON,
		READING_LINE_PARENTHESES,
		SKIP_TO_NEWLINE,
	};

	FString err(shaderError);
	size_t cur = 0;
	size_t state_start = 0;

	size_t line_start = 0;
	size_t num_end = 0;

	int state = READING_LUMP;

	int64_t lump_num = 0;

	while(cur < err.Len())
	{
		if(state != SKIP_TO_NEWLINE)
		{
			while(err[cur] >= '0' && err[cur] <= '9')
			{
				cur++;
			}

			if(cur == state_start)
			{
				state = SKIP_TO_NEWLINE;
			}
			else if(state == READING_LUMP && (err[cur] == '(' || err[cur] == ':'))
			{
				FString lump_num_str = err.Mid(state_start, cur - state_start);
				lump_num = lump_num_str.ToLong();
				line_start = state_start;
				state = (err[cur] == ':') ? READING_LINE_COLON : READING_LINE_PARENTHESES;
				cur++;
				state_start = cur;
			}
			else if((state == READING_LINE_COLON && err[cur] == ':') || (state == READING_LINE_PARENTHESES && err[cur] == ')'))
			{
				FString line_num_str = err.Mid(state_start, cur - state_start);

				if(state == READING_LINE_PARENTHESES)
				{
					cur+= 3; // skip ") :"
				}
				else
				{
					cur++; // skip ":"
				}

				// Only #include'd sources carry a lump number; errors in the main text report source string 0
				// (Mali: "0:123: L0001: ..."), which must not be used to index filenames_for_error.
				if (lump_num >= 1 && lump_num <= (int64_t)filenames_for_error.Size())
				{
					int64_t old_len = cur - line_start;
					FString new_err = "File '" + filenames_for_error[lump_num - 1] + "', Line " + line_num_str + ": ";

					int64_t diff = new_err.Len() - old_len;

					err = err.Left(line_start) + new_err + err.Mid(line_start + old_len);

					cur += diff;
				}
				state = SKIP_TO_NEWLINE;
			}
			else
			{ // couldn't find a valid num, skip line
				state = SKIP_TO_NEWLINE;
			}
		}
		//not 'else if' to allow this to run immediately after
		if(state == SKIP_TO_NEWLINE)
		{
			if(err[cur] == '\n' || err[cur] == '\r')
			{
				while(cur < err.Len() && (err[cur] == '\n' || err[cur] == '\r'))
				{
					cur++;
				}
				state_start = cur;
				state = READING_LUMP;
			}
			else
			{
				cur++;
			}
		}
	}
	return err;
}


// GPU instancing (hw_batching.h): per-instance attributes and the values handed to the fragment shader.
static const char *InstancingVertexDecl = R"(
#ifdef INSTANCING
layout(location = 9) in vec4 aInstXform0;
layout(location = 10) in vec4 aInstXform1;
layout(location = 11) in vec4 aInstXform2;
layout(location = 12) in vec4 aInstXform3;
layout(location = 13) in vec4 aInstLightAttr;
layout(location = 14) in vec4 aInstDynLight;
layout(location = 15) in uvec4 aInstColors;
flat out vec4 vInstLightAttr;
flat out vec4 vInstDynLight;
flat out vec4 vInstObjectColor;
flat out vec4 vInstAddColor;
flat out vec4 vInstFogColor;
flat out float vInstDesaturation;
#endif
)";

static const char *InstancingFragmentDecl = R"(
#ifdef INSTANCING
flat in vec4 vInstLightAttr;
flat in vec4 vInstDynLight;
flat in vec4 vInstObjectColor;
flat in vec4 vInstAddColor;
flat in vec4 vInstFogColor;
flat in float vInstDesaturation;
#define uLightAttr vInstLightAttr
#define uDynLightColor vInstDynLight
#define uObjectColor vInstObjectColor
#define uAddColor vInstAddColor
#define uFogColor vInstFogColor
#define uDesaturationFactor vInstDesaturation
#endif
)";

bool FShader::sCacheOnly = false;
bool FShader::sStartupDone = false;

// "<kind>|<hash of the defines>|<name>" of the shader the variant belongs to (a material shader with
// or without alpha test: the same name, other defines). kind: q/m instanced quads/models, c clip
// emulation, cq/cm instanced variants of the clip variant.
FString FShader::VariantId(const char *kind) const
{
	uint32_t hash = 2166136261u;
	for (const char *c = mDefines.GetChars(); c && *c; c++)
		hash = (hash ^ uint8_t(*c)) * 16777619u;
	FString id;
	id.Format("%s|%08x|%s", kind, hash, mName.GetChars());
	return id;
}

void FShader::RecordVariantUse(const char *kind) const
{
	const FShader *root = mParent ? mParent : this;
	ProgramBinaryCache::RecordVariant(ProgramCacheNamespace(), ProgramCacheDriver().GetChars(), root->VariantId(kind));
}

bool FShader::Load(const char * name, const char * vert_prog_lump, const char * frag_prog_lump, const char * proc_prog_lump, const char * light_fragprog, const char * defines, bool isGBuffer, AllShaderIndex type)
{
	FString error;

	if (!mParamsStored)
	{
		mVertLump = vert_prog_lump;
		mFragLump = frag_prog_lump;
		mHasProc = proc_prog_lump != nullptr;
		if (mHasProc) mProcLump = proc_prog_lump;
		mHasLight = light_fragprog != nullptr;
		if (mHasLight) mLightLump = light_fragprog;
		mDefines = defines;
		mIsGBuffer = isGBuffer;
		mType = type;
		mParamsStored = true;
	}

	FString i_data = R"(
		// these settings are actually pointless but there seem to be some old ATI drivers that fail to compile the shader without setting the precision here.
		precision highp int;
		precision highp float;
		precision highp sampler2DArray;

		uniform int uTextureMode;
		uniform vec2 uClipSplit;
		uniform float uAlphaThreshold;

		// colors
		uniform vec4 uObjectColor;
		uniform vec4 uObjectColor2;
		uniform vec4 uDynLightColor;
		uniform vec4 uLightProbeColor;
		uniform vec4 uAddColor;
		uniform vec4 uTextureBlendColor;
		uniform vec4 uTextureModulateColor;
		uniform vec4 uTextureAddColor;
		uniform vec4 uFogColor;
		uniform vec4 uSunDirection;
		uniform float uDesaturationFactor;
		uniform float uInterpolationFactor;
		uniform int uHeavyProbeSampleCount;
		uniform mat4 uHeavyProbeSampleColors;
		uniform mat4 uHeavyProbeSampleDirs;

		// Glowing walls stuff
		uniform vec4 uGlowTopPlane;
		uniform vec4 uGlowTopColor;
		uniform vec4 uGlowBottomPlane;
		uniform vec4 uGlowBottomColor;

		uniform vec4 uGradientTopPlane;
		uniform vec4 uGradientBottomPlane;

		uniform vec4 uSplitTopPlane;
		uniform vec4 uSplitBottomPlane;

		uniform vec4 uDetailParms;
		// Lighting + Fog
		uniform vec4 uLightAttr;
		#define uLightLevel uLightAttr.a
		#define uFogDensity uLightAttr.b
		#define uLightFactor uLightAttr.g
		#define uLightDist uLightAttr.r
		uniform int uFogEnabled;

		// dynamic lights
		uniform int uLightIndex;

		// bone animation
		uniform int uBoneIndexBase;

		// Blinn glossiness and specular level
		uniform vec2 uSpecularMaterial;

		// matrices
		uniform mat4 ModelMatrix;
		uniform mat4 NormalModelMatrix;
		uniform mat4 TextureMatrix;

		// light buffers
		#ifdef SHADER_STORAGE_LIGHTS
		layout(std430, binding = 1) buffer LightBufferSSO
		{
			vec4 lights[];
		};
		#elif defined NUM_UBO_LIGHTS
		uniform LightBufferUBO
		{
			vec4 lights[NUM_UBO_LIGHTS];
		};
		#endif

		// bone matrix buffers
		#ifdef SHADER_STORAGE_BONES
		layout(std430, binding = 7) buffer BoneBufferSSO
		{
			mat4 bones[];
		};
		#elif defined NUM_UBO_BONES
		uniform BoneBufferUBO
		{
			mat4 bones[NUM_UBO_BONES];
		};
		#endif

		// textures
		uniform sampler2D tex;
		uniform sampler2D ShadowMap;
		uniform sampler2DArray LightMap;
		uniform sampler2D texture2;
		uniform sampler2D texture3;
		uniform sampler2D texture4;
		uniform sampler2D texture5;
		uniform sampler2D texture6;
		uniform sampler2D texture7;
		uniform sampler2D texture8;
		uniform sampler2D texture9;
		uniform sampler2D texture10;
		uniform sampler2D texture11;
		uniform sampler2D texture12;

		// timer data
		uniform float timer;

		// material types
		#if defined(SPECULAR)
		#define normaltexture texture2
		#define speculartexture texture3
		#define brighttexture texture4
		#define detailtexture texture5
		#define glowtexture texture6
		#elif defined(PBR)
		#define normaltexture texture2
		#define metallictexture texture3
		#define roughnesstexture texture4
		#define aotexture texture5
		#define brighttexture texture6
		#define detailtexture texture7
		#define glowtexture texture8
		#else
		#define brighttexture texture2
		#define detailtexture texture3
		#define glowtexture texture4
		#endif

	)";


#ifdef __APPLE__
	// The noise functions are completely broken in macOS OpenGL drivers
	// Garbage values are returned, and their infrequent usage causes extreme slowdown
	// Also, these functions must return zeroes since GLSL 4.4
	i_data += "#define noise1(unused) 0.0\n";
	i_data += "#define noise2(unused) vec2(0)\n";
	i_data += "#define noise3(unused) vec3(0)\n";
	i_data += "#define noise4(unused) vec4(0)\n";
#endif // __APPLE__

#ifdef NPOT_EMULATION
	i_data += "#define NPOT_EMULATION\nuniform vec2 uNpotEmulation;\n";
#endif

	int vp_lump = fileSystem.CheckNumForFullName(vert_prog_lump, 0);
	if (vp_lump == -1) I_Error("Unable to load '%s'", vert_prog_lump);

	int fp_lump = fileSystem.CheckNumForFullName(frag_prog_lump, 0);
	if (fp_lump == -1) I_Error("Unable to load '%s'", frag_prog_lump);



//
// The following code uses GetChars on the strings to get rid of terminating 0 characters. Do not remove or the code may break!
//
	FString pre_placeholder;

	assert(screen->mLights != NULL);
	assert(screen->mBones != NULL);

#ifdef ANDROID    
    if (gEnableSpirvCross){
		pre_placeholder << "#version 410\n";
	}
	else {
		pre_placeholder << "#version 300 es\n";
		// ES fragment shaders have no default float precision, and the ViewpointUBO block below is the first
		// thing that needs one: without this Mali rejects every fragment shader
		// ("S0032: no default precision defined for member 'ProjectionMatrix'").
		pre_placeholder << "precision highp int;\nprecision highp float;\nprecision highp sampler2D;\nprecision highp sampler2DArray;\nprecision highp samplerCube;\nprecision highp sampler3D;\n";
	}

	bool lightbuffertype = screen->mLights->GetBufferType();
	if (!lightbuffertype)
		pre_placeholder.AppendFormat("#define NO_CLIPDISTANCE_SUPPORT\n#define NUM_UBO_LIGHTS %d\n#define NUM_UBO_BONES %d\n", screen->mLights->GetBlockSize(), screen->mBones->GetBlockSize());
	else
		pre_placeholder << "#define NO_CLIPDISTANCE_SUPPORT\n#define SHADER_STORAGE_LIGHTS\n#define SHADER_STORAGE_BONES\n";
#else    
	if ((gl.flags & RFL_SHADER_STORAGE_BUFFER) && screen->allowSSBO())
		pre_placeholder << "#version 430 core\n#define SUPPORTS_SHADOWMAPS\n";
	else
		pre_placeholder << "#version 330 core\n";

	bool lightbuffertype = screen->mLights->GetBufferType();
	if (!lightbuffertype)
		pre_placeholder.AppendFormat("#define NUM_UBO_LIGHTS %d\n#define NUM_UBO_BONES %d\n", screen->mLights->GetBlockSize(), screen->mBones->GetBlockSize());
	else
		pre_placeholder << "#define SHADER_STORAGE_LIGHTS\n#define SHADER_STORAGE_BONES\n";
#endif

	if(isGBuffer)
	{
		pre_placeholder << "\n#define GBUFFER_PASS\n";
	}

	pre_placeholder << "layout(std140) uniform ViewpointUBO" << ShaderInputsOutputs::GenerateStruct<HWViewpointUniforms>() << ";\n";

	FString vp_comb = defines;
	vp_comb << i_data.GetChars();
#ifdef ANDROID
	// The clip distance varyings only exist in the clip emulation variant.
	const bool hasClipDistance = !(defines && strstr(defines, "#define CLIP_EMULATION"));
#else
	const bool hasClipDistance = true;
#endif
	vp_comb << ShaderInputsOutputs::GenerateInputsOutputs(false, false, type, false, hasClipDistance);
	vp_comb << InstancingVertexDecl;
	FString fp_comb = defines;
	fp_comb << i_data.GetChars();
	fp_comb << ShaderInputsOutputs::GenerateInputsOutputs(false, true, type, isGBuffer, hasClipDistance);
	fp_comb << InstancingFragmentDecl;

	vp_comb << "#line 1\n";
	fp_comb << "#line 1\n";

	vp_comb << GetStringFromLump(vp_lump).GetChars() << "\n";
	fp_comb << GetStringFromLump(fp_lump).GetChars() << "\n";
	FString placeholder = "\n";
	TArray<FString> filenames_for_error;

	if (proc_prog_lump != NULL)
	{
		fp_comb << "#line 1\n";

		if (*proc_prog_lump != '#')
		{
			FString lump_filename(proc_prog_lump);
			FString pp_data;
			int pp_lump = fileSystem.CheckNumForFullName(proc_prog_lump, 0);	// if it's a core shader, ignore overrides by user mods.
			if (pp_lump == -1)
			{
				pp_lump = fileSystem.CheckNumForFullName(proc_prog_lump);
				if (pp_lump == -1)
				{
					I_Error("Unable to load '%s'", proc_prog_lump);
				}
				else
				{
					FString error = "";

					pp_data = stb_include_string(GetStringFromLump(pp_lump), lump_filename, filenames_for_error, error);

					if(!error.IsEmpty())
					{
						I_Error("Unable to load '%s': %s", proc_prog_lump, error.GetChars());
					}
				}
			}
			else
			{ // skip includes processing for code shaders
				pp_data = GetStringFromLump(pp_lump);
				filenames_for_error.Push(lump_filename);
			}

			if (pp_data.IndexOf("ProcessMaterial") < 0 && pp_data.IndexOf("SetupMaterial") < 0)
			{
				// this looks like an old custom hardware shader.

				if (pp_data.IndexOf("GetTexCoord") >= 0)
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultmat2.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultmat2.fp");
					fp_comb << "\n" << GetStringFromLump(pl_lump);
				}
				else
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultmat.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultmat.fp");
					fp_comb << "\n" << GetStringFromLump(pl_lump);

					if (pp_data.IndexOf("ProcessTexel") < 0)
					{
						// this looks like an even older custom hardware shader.
						// We need to replace the ProcessTexel call to make it work.
						placeholder << "#define NO_PROCESS_TEXEL\n";
					}
				}

				if (pp_data.IndexOf("ProcessLight") >= 0)
				{
					// The ProcessLight signatured changed. Forward to the old one.
					placeholder << "#define OLD_PROCESSLIGHT\n";
				}
			}

			fp_comb << RemoveLegacyUserUniforms(pp_data).GetChars();
			fp_comb.Substitute("gl_TexCoord[0]", "vTexCoord");	// fix old custom shaders.

			if (pp_data.IndexOf("ProcessLight") < 0)
			{
				int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultlight.fp", 0);
				if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultlight.fp");
				fp_comb << "\n" << GetStringFromLump(pl_lump);
			}

			// ProcessMaterial must be considered broken because it requires the user to fill in data they possibly cannot know all about.
			if (pp_data.IndexOf("ProcessMaterial") >= 0 && pp_data.IndexOf("SetupMaterial") < 0)
			{
				// This reactivates the old logic and disables all features that cannot be supported with that method.
				placeholder << "#define LEGACY_USER_SHADER\n";
			}
		}
		else
		{
			// Proc_prog_lump is not a lump name but the source itself (from generated shaders)
			fp_comb << proc_prog_lump + 1;
		}
	}
	fp_comb = pre_placeholder + placeholder + fp_comb;
	placeholder = "\n";
	vp_comb = pre_placeholder + placeholder + vp_comb;



	if (light_fragprog)
	{
		int pp_lump = fileSystem.CheckNumForFullName(light_fragprog, 0);
		if (pp_lump == -1) I_Error("Unable to load '%s'", light_fragprog);
		fp_comb << GetStringFromLump(pp_lump) << "\n";
	}

	if (gl.flags & RFL_NO_CLIP_PLANES)
	{
		// On ATI's GL3 drivers we have to disable gl_ClipDistance because it's hopelessly broken.
		// This will cause some glitches and regressions but is the only way to avoid total display garbage.
		vp_comb.Substitute("gl_ClipDistance", "//");
	}

#ifdef ANDROID
	if (sPrefetchOnly)
	{
		// Only start converting the sources of a program that is not in the binary cache; the
		// program is built from the same sources when the collection compiles it.
		uint32_t cachedFormat = 0;
		if (!IsShaderCacheActive() || LoadCachedProgramBinary(vp_comb, fp_comb, cachedFormat).Size() == 0)
		{
			QueueShaderConversion(vp_comb, true);
			QueueShaderConversion(fp_comb, false);
		}
		return false;
	}
#endif

	hShader = glCreateProgram();
	if (hShader != 0) sLivePrograms++;
	FGLDebug::LabelObject(GL_PROGRAM, hShader, name);

	uint32_t binaryFormat = 0;
	TArray<uint8_t> binary;
	if (IsShaderCacheActive())
		binary = LoadCachedProgramBinary(vp_comb, fp_comb, binaryFormat);

	bool linked = false;
	if (binary.Size() > 0 && glProgramBinary)
	{
		glProgramBinary(hShader, binaryFormat, binary.Data(), binary.Size());
		GLint status = 0;
		glGetProgramiv(hShader, GL_LINK_STATUS, &status);
		linked = (status == GL_TRUE);
		if (!linked)
			RejectCachedProgramBinary(vp_comb, fp_comb);
	}

	if (!linked && sCacheOnly)
	{
		// only looked up in the cache (variant prewarm): not there, nothing is compiled now
		glDeleteProgram(hShader);
		sLivePrograms--;
		hShader = 0;
		return false;
	}

	if (!linked)
	{
		hVertProg = glCreateShader(GL_VERTEX_SHADER);
		hFragProg = glCreateShader(GL_FRAGMENT_SHADER);

		FGLDebug::LabelObject(GL_SHADER, hVertProg, vert_prog_lump);
		FGLDebug::LabelObject(GL_SHADER, hFragProg, frag_prog_lump);
#if ANDROID
        if (gEnableSpirvCross) {
            const FString spirv_vp_comb = ConvertShaderToGLES(vp_comb.GetChars(), true);
            const FString spirv_fp_comb = ConvertShaderToGLES(fp_comb.GetChars(), false);
            const int vp_size = (int)spirv_vp_comb.Len();
            const int fp_size = (int)spirv_fp_comb.Len();
            const char *spirv_vp_ptr = spirv_vp_comb.GetChars();
            const char *spirv_fp_ptr = spirv_fp_comb.GetChars();
            glShaderSource(hVertProg, 1, &spirv_vp_ptr, &vp_size);
            glShaderSource(hFragProg, 1, &spirv_fp_ptr, &fp_size);
        } else {
#endif
            int vp_size = (int) vp_comb.Len();
            int fp_size = (int) fp_comb.Len();

            const char *vp_ptr = vp_comb.GetChars();
            const char *fp_ptr = fp_comb.GetChars();

            glShaderSource(hVertProg, 1, &vp_ptr, &vp_size);
            glShaderSource(hFragProg, 1, &fp_ptr, &fp_size);
#if ANDROID
        }
#endif
		GLint status = 0;

		bool errored = false;

		glCompileShader(hVertProg);

		if (glGetShaderiv(hVertProg, GL_COMPILE_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetShaderiv(hVertProg, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetShaderInfoLog(hVertProg, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				//error << "Vertex shader:\n" << buffer.Data() << "\n";
				error << "Vertex shader:\n" << ProcessShaderError(buffer.Data(), filenames_for_error) << "\n";
			}

			errored = true;
		}

		glCompileShader(hFragProg);

		if (glGetShaderiv(hFragProg, GL_COMPILE_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetShaderiv(hFragProg, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetShaderInfoLog(hFragProg, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				error << "Fragment shader:\n" << ProcessShaderError(buffer.Data(), filenames_for_error) << "\n";
			}

			errored = true;
		}

		if(errored)
		{
			// only print message if there's an error.
			I_Error("Errors Compiliong Shader '%s':\n%s\n", name, error.GetChars());
		}

		glAttachShader(hShader, hVertProg);
		glAttachShader(hShader, hFragProg);

		if (glProgramParameteri && IsShaderCacheActive())
			glProgramParameteri(hShader, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glLinkProgram(hShader);

		if (glGetProgramiv(hShader, GL_LINK_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetProgramiv(hShader, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetProgramInfoLog(hShader, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				error << "Linking:\n" << buffer.Data() << "\n";
			}

			I_Error("Errors Linking Shader '%s':\n%s\n", name, error.GetChars());
		}
		else if (glProgramBinary && IsShaderCacheActive())
		{
			int binaryLength = 0;
			glGetProgramiv(hShader, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
			binary.Resize(binaryLength);
			if (binaryLength > 0)
			{
				binaryLength = 0;
				glGetProgramBinary(hShader, binary.Size(), &binaryLength, &binaryFormat, binary.Data());
				binary.Resize(binaryLength > 0 ? binaryLength : 0);
				SaveCachedProgramBinary(vp_comb, fp_comb, binary, binaryFormat);
			}
		}
		// The linked program keeps all it needs: the shader objects would only hold on to their
		// source and their compiled form (under ANGLE a copy of the SPIR-V) as long as the program lives.
		glDetachShader(hShader, hVertProg);
		glDetachShader(hShader, hFragProg);
		glDeleteShader(hVertProg);
		glDeleteShader(hFragProg);
	}
	hVertProg = 0;
	hFragProg = 0;

	muDesaturation.Init(hShader, "uDesaturationFactor");
	muFogEnabled.Init(hShader, "uFogEnabled");
	muTextureMode.Init(hShader, "uTextureMode");
	muLightParms.Init(hShader, "uLightAttr");
	muClipSplit.Init(hShader, "uClipSplit");
	muLightIndex.Init(hShader, "uLightIndex");
	muBoneIndexBase.Init(hShader, "uBoneIndexBase");
	muFogColor.Init(hShader, "uFogColor");
	muSunDirection.Init(hShader, "uSunDirection");
	muDynLightColor.Init(hShader, "uDynLightColor");
	muLightProbeColor.Init(hShader, "uLightProbeColor");
	muObjectColor.Init(hShader, "uObjectColor");
	muObjectColor2.Init(hShader, "uObjectColor2");
	muGlowBottomColor.Init(hShader, "uGlowBottomColor");
	muGlowTopColor.Init(hShader, "uGlowTopColor");
	muGlowBottomPlane.Init(hShader, "uGlowBottomPlane");
	muGlowTopPlane.Init(hShader, "uGlowTopPlane");
	muGradientBottomPlane.Init(hShader, "uGradientBottomPlane");
	muGradientTopPlane.Init(hShader, "uGradientTopPlane");
	muSplitBottomPlane.Init(hShader, "uSplitBottomPlane");
	muSplitTopPlane.Init(hShader, "uSplitTopPlane");
	muDetailParms.Init(hShader, "uDetailParms");
#ifdef NPOT_EMULATION
	muNpotEmulation.Init(hShader, "uNpotEmulation");
#endif
	muInterpolationFactor.Init(hShader, "uInterpolationFactor");
	muAlphaThreshold.Init(hShader, "uAlphaThreshold");
	muSpecularMaterial.Init(hShader, "uSpecularMaterial");
	muAddColor.Init(hShader, "uAddColor");
	muTextureAddColor.Init(hShader, "uTextureAddColor");
	muTextureModulateColor.Init(hShader, "uTextureModulateColor");
	muTextureBlendColor.Init(hShader, "uTextureBlendColor");
	muTimer.Init(hShader, "timer");

	muHeavyProbeSampleCount.Init(hShader, "uHeavyProbeSampleCount");
	muHeavyProbeSampleColors.Init(hShader, "uHeavyProbeSampleColors");
	muHeavyProbeSampleDirs.Init(hShader, "uHeavyProbeSampleDirs");

	lights_index = glGetUniformLocation(hShader, "lights");
	modelmatrix_index = glGetUniformLocation(hShader, "ModelMatrix");
	texturematrix_index = glGetUniformLocation(hShader, "TextureMatrix");
	normalmodelmatrix_index = glGetUniformLocation(hShader, "NormalModelMatrix");

	if (!lightbuffertype)
	{
		int tempindex = glGetUniformBlockIndex(hShader, "LightBufferUBO");
		if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, LIGHTBUF_BINDINGPOINT);

		tempindex = glGetUniformBlockIndex(hShader, "BoneBufferUBO");
		if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, BONEBUF_BINDINGPOINT);
	}
	int tempindex = glGetUniformBlockIndex(hShader, "ViewpointUBO");
	if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, VIEWPOINT_BINDINGPOINT);

	glUseProgram(hShader);

	// set up other texture units (if needed by the shader)
	for (int i = 2; i<16; i++)
	{
		char stringbuf[20];
		mysnprintf(stringbuf, 20, "texture%d", i);
		tempindex = glGetUniformLocation(hShader, stringbuf);
		if (tempindex != -1) glUniform1i(tempindex, i - 1);
	}

	int shadowmapindex = glGetUniformLocation(hShader, "ShadowMap");
	if (shadowmapindex != -1) glUniform1i(shadowmapindex, 16);

	int lightmapindex = glGetUniformLocation(hShader, "LightMap");
	if (lightmapindex != -1) glUniform1i(lightmapindex, 17);

	glUseProgram(0);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

int FShader::sLivePrograms = 0;

FShader::~FShader()
{
	if (hShader != 0) sLivePrograms--;
	for (auto variant : mInstanced) delete variant;
	delete mClipVariant;
	for (auto variant : mLightBlendVariant) delete variant;
	glDeleteProgram(hShader);
	if (hVertProg != 0)
		glDeleteShader(hVertProg);
	if (hFragProg != 0)
		glDeleteShader(hFragProg);
}


//==========================================================================
//
//
//
//==========================================================================

FShader *FShader::BuildVariant(const FString &extraDefines, const char *what)
{
	FString defines = mDefines;
	defines << extraDefines;
	auto shader = new FShader(mName.GetChars());
	try
	{
		if (!shader->Load(mName.GetChars(), mVertLump.GetChars(), mFragLump.GetChars(), mHasProc ? mProcLump.GetChars() : nullptr,
			mHasLight ? mLightLump.GetChars() : nullptr, defines.GetChars(), mIsGBuffer, mType))
		{
			delete shader;
			shader = nullptr;
		}
	}
	catch (CRecoverableError &err)
	{
		Printf("%s variant of shader %s unavailable:\n%s\n", what, mName.GetChars(), err.GetMessage());
		delete shader;
		shader = nullptr;
	}
	// Load() leaves no program bound; make the next Bind() rebind for real.
	GLRenderer->mShaderManager->SetActiveShader(nullptr);
	return shader;
}

FShader *FShader::GetInstancedVariant(int variant)
{
	if (variant < 1 || variant > 3 || !mParamsStored) return nullptr;
	int slot = variant - 1;
	if (mInstanced[slot] || mInstancedFailed[slot]) return mInstanced[slot];

	FString defines;
	static const char *const kindDefines[3] = { "#define INSTANCED_QUADS\n", "#define INSTANCED_MODELS\n", "#define INSTANCED_WALLS\n" };
	defines << "#define INSTANCING\n" << kindDefines[slot];
	FShader *shader = BuildVariant(defines, "Instanced");	// nullptr: drawn without instancing
	if (shader == nullptr && sCacheOnly)
		return nullptr;		// not in the cache: built on first use
	if (shader)
	{
		shader->mIsClipVariant = mIsClipVariant;
		shader->mParent = mParent;
		if (!sCacheOnly)
		{
			static const char *const kinds[2][3] = { { "q", "m", "w" }, { "cq", "cm", "cw" } };
			RecordVariantUse(kinds[mIsClipVariant ? 1 : 0][slot]);
		}
	}
	mInstanced[slot] = shader;
	mInstancedFailed[slot] = shader == nullptr;
	return shader;
}

FShader *FShader::GetClipVariant()
{
	if (mIsClipVariant) return this;
	if (mClipVariant) return mClipVariant;
	if (mClipVariantFailed || !mParamsStored) return this;
	// The emulation is in main.fp; effects with their own fragment program (fog boundary, burn,
	// stencil) have none, a variant of them would only add the varyings.
	if (mFragLump.CompareNoCase("shaders/glsl/main.fp") != 0) return this;

	FShader *shader = BuildVariant("#define CLIP_EMULATION\n", "Clip emulation");
	if (shader == nullptr && sCacheOnly)
		return this;		// not in the cache: built on first use
	if (shader)
	{
		shader->mIsClipVariant = true;
		shader->mParent = this;
		if (!sCacheOnly)
			RecordVariantUse("c");
	}
	mClipVariant = shader;
	mClipVariantFailed = shader == nullptr;
	return shader ? shader : this;
}

FShader *FShader::GetLightBlendVariant(int mode)
{
	if (mode < 1 || mode > 2 || mLightBlendMode != 0 || mIsClipVariant || !mParamsStored) return this;
	// Only the light models that apply it (material_normal.fp, material_specular.fp): for the others
	// the variant would be the same program.
	if (!mHasLight || (mLightLump.IndexOf("material_normal") < 0 && mLightLump.IndexOf("material_specular") < 0)) return this;

	const int slot = mode - 1;
	if (mLightBlendVariant[slot]) return mLightBlendVariant[slot];
	if (mLightBlendFailed[slot]) return this;

	FString defines;
	defines.Format("#define LIGHT_BLEND_MODE %d\n", mode);
	FShader *shader = BuildVariant(defines, "Light blend mode");
	if (shader == nullptr && sCacheOnly)
		return this;		// not in the cache: built on first use
	if (shader)
	{
		// Its own clip and instanced variants are recorded under it (its defines tell it apart)
		shader->mLightBlendMode = mode;
		if (!sCacheOnly)
			RecordVariantUse(mode == 1 ? "b1" : "b2");
	}
	mLightBlendVariant[slot] = shader;
	mLightBlendFailed[slot] = shader == nullptr;
	return shader ? shader : this;	// drawn with the default light blend mode if it cannot be built
}

int FShader::PrewarmVariants(const TArray<FString> &recorded)
{
	if (!mParamsStored || mIsClipVariant || recorded.Size() == 0)
		return 0;
	auto used = [&](const char *kind)
	{
		const FString id = VariantId(kind);
		for (auto &entry : recorded)
			if (entry.Compare(id) == 0) return true;
		return false;
	};
	int loaded = 0;
	sCacheOnly = true;
	if (used("q") && GetInstancedVariant(1)) loaded++;
	if (used("m") && GetInstancedVariant(2)) loaded++;
	if (used("w") && GetInstancedVariant(3)) loaded++;
	if (used("c") || used("cq") || used("cm") || used("cw"))
	{
		FShader *clip = GetClipVariant();
		if (clip != this)
		{
			loaded++;
			if (used("cq") && clip->GetInstancedVariant(1)) loaded++;
			if (used("cm") && clip->GetInstancedVariant(2)) loaded++;
			if (used("cw") && clip->GetInstancedVariant(3)) loaded++;
		}
	}
	sCacheOnly = false;

	// The light blend variants, then theirs (recorded under the light blend variant)
	for (int mode = 1; mode <= 2; mode++)
	{
		if (!used(mode == 1 ? "b1" : "b2")) continue;
		sCacheOnly = true;
		FShader *blend = GetLightBlendVariant(mode);
		sCacheOnly = false;
		if (blend != this)
		{
			loaded++;
			loaded += blend->PrewarmVariants(recorded);
		}
	}
	return loaded;
}

bool FShader::Bind()
{
	GLRenderer->mShaderManager->SetActiveShader(this);
	return true;
}

//==========================================================================
//
// Since all shaders are REQUIRED, any error here needs to be fatal
//
//==========================================================================

static FString MaterialDefines(const char *shaderdefines, bool usediscard)
{
	FString defines;
	if (shaderdefines) defines += shaderdefines;
	// this can't be in the shader code due to ATI strangeness.
	if (!usediscard) defines += "#define NO_ALPHATEST\n";

#ifdef ANDROID
	if(gl_lite_shader)
		defines += "#define SHADER_LITE\n";
#endif
	return defines;
}

int FShaderCollection::ProgramCount() const
{
	int count = 0;
	while (defaultshaders[count].ShaderName != nullptr) count++;
	count += SHADER_NoTexture + MAX_EFFECTS;
#ifdef ANDROID
	if (gl_customshader)
#endif
		count += usershaders.Size();
	return count;
}

#ifdef ANDROID
bool FShader::sPrefetchOnly = false;

//==========================================================================
//
// The same programs as CompileNextShader, with the same sources: Load() in
// prefetch mode assembles them and queues their conversion.
//
//==========================================================================

void FShaderCollection::PrefetchSources()
{
	if (!gEnableSpirvCross) return;

	auto prefetch = [this](const char *name, const char *vp, const char *fp, const char *proc, const char *light, const FString &defines, int type)
	{
		FShader shader(name);
		try
		{
			shader.Load(name, vp, fp, proc, light, defines.GetChars(), mPassType == GBUFFER_PASS, static_cast<AllShaderIndex>(type));
		}
		catch (CRecoverableError &)
		{
			// reported when the program itself is compiled
		}
	};

	FShader::sPrefetchOnly = true;
	for (int i = 0; defaultshaders[i].ShaderName != nullptr; i++)
	{
		prefetch(defaultshaders[i].ShaderName, "shaders/glsl/main.vp", "shaders/glsl/main.fp", defaultshaders[i].gettexelfunc,
			defaultshaders[i].lightfunc, MaterialDefines(defaultshaders[i].Defines, true), i);
	}
	for (int i = 0; i < SHADER_NoTexture; i++)
	{
		prefetch(defaultshaders[i].ShaderName, "shaders/glsl/main.vp", "shaders/glsl/main.fp", defaultshaders[i].gettexelfunc,
			defaultshaders[i].lightfunc, MaterialDefines(defaultshaders[i].Defines, false), i);
	}
	if (gl_customshader)
	{
		for (auto &shader : usershaders)
		{
			FString name = ExtractFileBase(shader.shader.GetChars());
			FString defines = defaultshaders[shader.shaderType].Defines + shader.defines;
			prefetch(name.GetChars(), "shaders/glsl/main.vp", "shaders/glsl/main.fp", shader.shader.GetChars(),
				defaultshaders[shader.shaderType].lightfunc, MaterialDefines(defines.GetChars(), true), shader.shaderType);
		}
	}
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		prefetch(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3,
			FString(effectshaders[i].defines), i + FIRST_EFFECT_SHADER);
	}
	FShader::sPrefetchOnly = false;
}
#endif

FShader *FShaderCollection::Compile (const char *ShaderName, const char *ShaderPath, const char *LightModePath, const char *shaderdefines, bool usediscard, EPassType passType, AllShaderIndex type)
{
	FString defines = MaterialDefines(shaderdefines, usediscard);

	FShader *shader = NULL;
	try
	{
		shader = new FShader(ShaderName);
		if (!shader->Load(ShaderName, "shaders/glsl/main.vp", "shaders/glsl/main.fp", ShaderPath, LightModePath, defines.GetChars(), (passType == GBUFFER_PASS), type))
		{
			I_FatalError("Unable to load shader %s\n", ShaderName);
		}
	}
	catch(CRecoverableError &err)
	{
		if (shader != NULL) delete shader;
		shader = NULL;
		I_FatalError("Unable to load shader %s:\n%s\n", ShaderName, err.GetMessage());
	}
	return shader;
}

//==========================================================================
//
//
//
//==========================================================================

FShaderManager::FShaderManager()
{
	for (int passType = 0; passType < MAX_PASS_TYPES; passType++)
		mPassShaders.Push(new FShaderCollection((EPassType)passType));
}

int FShaderCollection::PrewarmVariants(const TArray<FString> &recorded)
{
	int loaded = 0;
	for (auto shader : mMaterialShaders)
		if (shader) loaded += shader->PrewarmVariants(recorded);
	for (auto shader : mMaterialShadersNAT)
		if (shader) loaded += shader->PrewarmVariants(recorded);
	return loaded;
}

bool FShaderManager::CompileNextShader()
{
	if (mCompilePass < 0) return true;
	bool done = false;
	if (mPassShaders[mCompilePass]->CompileNextShader())
	{
		mCompilePass++;
		// The G-buffer pass is drawn only with SSAO; without it, it is compiled when first used.
		if (mCompilePass >= MAX_PASS_TYPES || (mCompilePass == GBUFFER_PASS && gl_ssao == 0))
		{
			mCompilePass = -1;
#ifdef ANDROID
			FinishShaderConversions();
			// the variants earlier runs built on first use come from the binary cache now
			mPassShaders[NORMAL_PASS]->PrewarmVariants(ProgramBinaryCache::RecordedVariants(ProgramCacheNamespace(), ProgramCacheDriver().GetChars()));
			FShader::sStartupDone = true;
#endif
			done = true;
		}
	}
	// Load() leaves no program bound: the start screen drawn between programs must rebind.
	SetActiveShader(nullptr);
	return done;
}

int FShaderManager::StartupProgramCount() const
{
	return (gl_ssao == 0 ? 1 : MAX_PASS_TYPES) * mPassShaders[NORMAL_PASS]->ProgramCount();
}

FShaderCollection *FShaderManager::CompiledCollection(EPassType passType)
{
	FShaderCollection *collection = mPassShaders[passType];
	if (!collection->IsCompiled())
	{
		// SSAO turned on after the startup: the G-buffer pass is built on its first use.
		Printf("Compiling the %s pass shaders...\n", passType == GBUFFER_PASS ? "G-buffer" : "scene");
		while (!collection->CompileNextShader()) {}
#ifdef ANDROID
		FinishShaderConversions();
#endif
		SetActiveShader(nullptr);
	}
	return collection;
}

FShaderManager::~FShaderManager()
{
	glUseProgram(0);
	mActiveShader = NULL;

	for (auto collection : mPassShaders)
		delete collection;
}

void FShaderManager::SetActiveShader(FShader *sh)
{
	if (mActiveShader != sh)
	{
		glUseProgram(sh!= NULL? sh->GetHandle() : 0);
		mActiveShader = sh;
	}
}

FShader *FShaderManager::BindEffect(int effect, EPassType passType)
{
	if (passType < mPassShaders.Size() && mCompilePass == -1)
		return CompiledCollection(passType)->BindEffect(effect);
	else
		return nullptr;
}

FShader *FShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (mCompilePass > -1)
	{
		return mPassShaders[0]->Get(0, false);
	}
	if ((r_skipmats && eff >= 3 && eff <= 4))
		eff = 0;

	if (passType < mPassShaders.Size())
		return CompiledCollection(passType)->Get(eff, alphateston);
	else
		return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FShaderCollection::FShaderCollection(EPassType passType)
{
	mPassType = passType;
	mMaterialShaders.Clear();
	mMaterialShadersNAT.Clear();
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		mEffectShaders[i] = NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FShaderCollection::~FShaderCollection()
{
	Clean();
}

//==========================================================================
//
//
//
//==========================================================================

bool FShaderCollection::CompileNextShader()
{
	if (mCompileState == -1) return true;
#ifdef ANDROID
	if (mCompileState == 0 && mCompileIndex == 0) PrefetchSources();
#endif
	int i = mCompileIndex;
	if (mCompileState == 0)
	{ // regular shaders
		FShader *shc = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, true, mPassType, static_cast<AllShaderIndex>(i));
		mMaterialShaders.Push(shc);
		mCompileIndex++;
		if (defaultshaders[mCompileIndex].ShaderName == nullptr)
		{
			mCompileIndex = 0;
			mCompileState++;

		}
	}
	else if (mCompileState == 1)
	{ // noalphatest shaders
		FShader *shc1 = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, false, mPassType, static_cast<AllShaderIndex>(i));
		mMaterialShadersNAT.Push(shc1);
		mCompileIndex++;
		if (mCompileIndex >= SHADER_NoTexture)
		{
			mCompileIndex = 0;
			mCompileState++;
#ifdef ANDROID
			if (usershaders.Size() == 0 || !gl_customshader) mCompileState++;
#else
			if (usershaders.Size() == 0) mCompileState++;
#endif
		}
	}
	else if (mCompileState == 2)
	{ // user shaders
		auto &shader = usershaders[i];
		FString name = ExtractFileBase(shader.shader.GetChars());
		FString defines = defaultshaders[shader.shaderType].Defines + shader.defines;
		FShader *shc = Compile(name.GetChars(), shader.shader.GetChars(), defaultshaders[shader.shaderType].lightfunc, defines.GetChars(), true, mPassType, static_cast<AllShaderIndex>(shader.shaderType));
		mMaterialShaders.Push(shc);
		mCompileIndex++;
		if (mCompileIndex >= (int)usershaders.Size())
		{
			mCompileIndex = 0;
			mCompileState++;
		}
	}
	else if (mCompileState == 3)
	{ // effect shaders
		FShader *eff = new FShader(effectshaders[i].ShaderName);
		if (!eff->Load(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1,
						effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines, (mPassType == GBUFFER_PASS), static_cast<AllShaderIndex>(i + FIRST_EFFECT_SHADER)))
		{
			delete eff;
		}
		else mEffectShaders[i] = eff;
		mCompileIndex++;
		if (mCompileIndex >= MAX_EFFECTS)
		{
			// Get() hands out the shaders without alpha test only from here on. Nothing set this,
			// so opaque geometry was always drawn with the alpha-testing (discarding) shaders,
			// which costs early depth testing / hidden surface removal on tile-based GPUs.
			mCompileState = -1;
			return true;
		}
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

void FShaderCollection::Clean()
{
	for (unsigned int i = 0; i < mMaterialShadersNAT.Size(); i++)
	{
		if (mMaterialShadersNAT[i] != NULL) delete mMaterialShadersNAT[i];
	}
	for (unsigned int i = 0; i < mMaterialShaders.Size(); i++)
	{
		if (mMaterialShaders[i] != NULL) delete mMaterialShaders[i];
	}
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		if (mEffectShaders[i] != NULL) delete mEffectShaders[i];
		mEffectShaders[i] = NULL;
	}
	mMaterialShaders.Clear();
	mMaterialShadersNAT.Clear();
}

//==========================================================================
//
//
//
//==========================================================================

int FShaderCollection::Find(const char * shn)
{
	FName sfn = shn;

	for(unsigned int i=0;i<mMaterialShaders.Size();i++)
	{
		if (mMaterialShaders[i]->mName == sfn)
		{
			return i;
		}
	}
	return -1;
}


//==========================================================================
//
//
//
//==========================================================================

FShader *FShaderCollection::BindEffect(int effect)
{
	if (effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[effect] != NULL)
	{
		mEffectShaders[effect]->Bind();
		return mEffectShaders[effect];
	}
	return NULL;
}


//==========================================================================
//
//
//
//==========================================================================

void gl_DestroyUserShaders()
{
	// todo
}
}

#define GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#define GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#define GPU_MEMORY_INFO_EVICTION_COUNT_NVX 0x904A
#define GPU_MEMORY_INFO_EVICTED_MEMORY_NVX 0x904B

#define VBO_FREE_MEMORY_ATI 0x87FB
#define TEXTURE_FREE_MEMORY_ATI 0x87FC
#define RENDERBUFFER_FREE_MEMORY_ATI 0x87FD

static FString FormatKB(int kb)
{
	int gb = kb / (1024 * 1024);
	kb -= gb * (1024 * 1024);
	int mb = kb / 1024;
	kb -= mb * 1024;
	FString tmp = "";
	tmp.Format("%d GB %d MB %d KB", gb, mb, kb);
	return tmp;
}

void PrintVRAM_NV(FString &out)
{
	int dedicatedMemoryNVidia = -1;
	glGetIntegerv(GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicatedMemoryNVidia);
	int totalMemoryNVidia = -1;
	glGetIntegerv(GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &totalMemoryNVidia);
	int freeMemoryNVidia = -1;
	glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &freeMemoryNVidia);
	int evictionCountNVidia = -1;
	glGetIntegerv(GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &evictionCountNVidia);
	int evictedMemoryNVidia = -1;
	glGetIntegerv(GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &evictedMemoryNVidia);

	out.AppendFormat("Dedicated VRAM: %s\n", FormatKB(dedicatedMemoryNVidia).GetChars());
	out.AppendFormat("Total VRAM: %s\n", FormatKB(totalMemoryNVidia).GetChars());
	out.AppendFormat("Free VRAM: %s\n", FormatKB(freeMemoryNVidia).GetChars());
	out.AppendFormat("Eviction Count: %d\n", evictionCountNVidia);
	out.AppendFormat("Evicted Memory: %s", FormatKB(evictedMemoryNVidia).GetChars());
}

void PrintVRAM_ATI(FString &out)
{
	struct gpu_memory_info_t
	{
		int total_free = -1;
		int largest_block = -1;
		int total_aux_free = -1;
		int largest_aux_block = -1;
	};

	gpu_memory_info_t texture_free_memory;

	glGetIntegerv(TEXTURE_FREE_MEMORY_ATI, &texture_free_memory.total_free);

	out.AppendFormat("VRAM:\n");
	out.AppendFormat("    Total Free: %s\n", FormatKB(texture_free_memory.total_free).GetChars());
	out.AppendFormat("    Largest Free Block: %s\n", FormatKB(texture_free_memory.largest_block).GetChars());
	out.AppendFormat("    Total Aux. Free: %s\n", FormatKB(texture_free_memory.total_aux_free).GetChars());
	out.AppendFormat("    Largest Free Aux. Block: %s\n", FormatKB(texture_free_memory.largest_aux_block).GetChars());
}

