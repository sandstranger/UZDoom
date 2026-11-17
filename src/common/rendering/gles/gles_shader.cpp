/*
** gles_shader.cpp
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

#include "gles_system.h"
#include "c_cvars.h"
#include "v_video.h"
#include "filesystem.h"
#include "engineerrors.h"
#include "cmdlib.h"
#include "md5.h"
#include "gles_shader.h"
#include "hw_shaderpatcher.h"
#include "shaderuniforms.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "i_specialpaths.h"
#include "printf.h"
#include "version.h"
#include "stb_include.h"

#include "matrix.h"
#include "gles_renderer.h"
#include "hw_programcache.h"
#include "hw_batching.h"
#include <map>
#include <memory>
#include <string>

EXTERN_CVAR(Bool, gl_customshader)	// hw_material.cpp (global)

namespace OpenGLESRenderer
{

CVAR(Int, gles_glsl_precision, 2, 0); // 0 = low, 1 = medium, 2 = high

FString GetGLSLPrecision()
{
	// GLSL ES 1.00 fragment shaders only have highp where GL_FRAGMENT_PRECISION_HIGH is defined
	// (not on e.g. Mali-400): fall back to mediump there. The macro is visible in both stages,
	// so uniforms shared by the vertex and fragment shader keep matching precisions (a link
	// requirement in GLSL ES 1.00). GLSL ES 3.00 always has highp.
	FString str = "#if defined(GL_FRAGMENT_PRECISION_HIGH) || __VERSION__ >= 300\n"
		"precision highp int;\nprecision highp float;\n"
		"#else\n"
		"precision mediump int;\nprecision mediump float;\n"
		"#endif\n";

	if (gles_glsl_precision == 0)
		str.Substitute("highp", "lowp");
	else if (gles_glsl_precision == 1)
		str.Substitute("highp", "mediump");

	return str;
}

bool IsShaderCacheActive()
{
	if (!glProgramBinary || !glGetProgramBinary)
		return false;
#ifdef ANDROID
	// GLES 2.0 path: glProgramBinary is ES 3.0 core; the OES entry points are not loaded.
	if (gles.glesMode != GLES_MODE_GLES3)
		return false;
	// ES 3.0 core, but a driver may expose zero binary formats (then every binary is refused).
	static int numFormats = -1;
	if (numFormats < 0)
	{
		GLint n = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
		numFormats = n > 0 ? n : 0;
	}
	return numFormats > 0;
#else
	static bool active = true;
	static bool firstcall = true;

	if (firstcall)
	{
		const char *vendor = (const char *)glGetString(GL_VENDOR);
		active = !(strstr(vendor, "Intel") == nullptr);
		firstcall = false;
	}
	return active;
#endif
}

// Program binaries live in the shared per-driver cache (hw_programcache.cpp); GLES2-style and
// GLES3-style sources of the same shader get separate namespaces.
static FString ProgramCacheNamespace()
{
	FString ns;
	ns.Format("gles-mode%d", (int)gles.glesMode);
	return ns;
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
	return ProgramBinaryCache::MakeKey(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars(), GetGitHash(), vertex, fragment);
}

TArray<uint8_t> LoadCachedProgramBinary(const FString &vertex, const FString &fragment, uint32_t &binaryFormat)
{
	TArray<uint8_t> data;
	if (!ProgramBinaryCache::Load(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment), binaryFormat, data))
		binaryFormat = 0;
	return data;
}

void SaveCachedProgramBinary(const FString &vertex, const FString &fragment, const TArray<uint8_t> &binary, uint32_t binaryFormat)
{
	ProgramBinaryCache::Store(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment), binaryFormat, binary);
}

void RejectCachedProgramBinary(const FString &vertex, const FString &fragment)
{
	ProgramBinaryCache::Reject(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars(), ProgramCacheKey(vertex, fragment));
}

#ifdef ANDROID
}

namespace OpenGLRenderer
{
	// gl_shader.cpp: NG-GL4ES's GLSL -> GLSL ES converter, shared with the GL renderer
	bool TryLoadShaderConverter();
	std::string ConvertShaderToGLES(const char *shaderSource, bool isVertexShader);
}

namespace OpenGLESRenderer
{

// Mods' material shaders the driver refuses go through NG-GL4ES when it can be loaded (checked once)
static bool UseMaterialShaderConverter()
{
	static int available = -1;
	if (available < 0)
	{
		available = OpenGLRenderer::TryLoadShaderConverter() ? 1 : 0;
		if (!available) Printf("NG-GL4ES not available: mods' material shaders are used as GLSL ES 3.00 only\n");
	}
	return available == 1;
}
#endif

//==========================================================================
//
// Compiles and links a program of the main shader (vertex attributes bound
// to their fixed locations). On failure 'error' gets the compiler's and the
// linker's messages and the program has no shaders attached.
//
//==========================================================================

static bool BuildProgram(GLuint program, const FString &vertexSource, const FString &fragmentSource, FString &error)
{
	static char buffer[10000];

	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

	int vp_size = (int)vertexSource.Len();
	int fp_size = (int)fragmentSource.Len();
	const char *vp_ptr = vertexSource.GetChars();
	const char *fp_ptr = fragmentSource.GetChars();

	glShaderSource(vertexShader, 1, &vp_ptr, &vp_size);
	glShaderSource(fragmentShader, 1, &fp_ptr, &fp_size);

	glCompileShader(vertexShader);
	glCompileShader(fragmentShader);

	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);

	glBindAttribLocation(program, VATTR_VERTEX, "aPosition");
	glBindAttribLocation(program, VATTR_TEXCOORD, "aTexCoord");
	glBindAttribLocation(program, VATTR_COLOR, "aColor");
	glBindAttribLocation(program, VATTR_VERTEX2, "aVertex2");
	glBindAttribLocation(program, VATTR_NORMAL, "aNormal");
	glBindAttribLocation(program, VATTR_NORMAL2, "aNormal2");
	glBindAttribLocation(program, VATTR_BONEWEIGHT, "aBoneWeight");
	glBindAttribLocation(program, VATTR_BONESELECTOR, "aBoneSelector");
	// instance attributes (only present in the instanced flavours)
	glBindAttribLocation(program, VATTR_INSTANCE0 + 0, "aInstXform0");
	glBindAttribLocation(program, VATTR_INSTANCE0 + 1, "aInstXform1");
	glBindAttribLocation(program, VATTR_INSTANCE0 + 2, "aInstXform2");
	glBindAttribLocation(program, VATTR_INSTANCE0 + 3, "aInstXform3");
	glBindAttribLocation(program, VATTR_INSTANCE_LIGHT, "aInstLightAttr");
	glBindAttribLocation(program, VATTR_INSTANCE_DYNLIGHT, "aInstDynLight");
	glBindAttribLocation(program, VATTR_INSTANCE_COLORS, "aInstColors");

	if (IsShaderCacheActive() && glProgramParameteri)
		glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glLinkProgram(program);

	buffer[0] = 0;
	glGetShaderInfoLog(vertexShader, sizeof(buffer), NULL, buffer);
	if (*buffer) error << "Vertex shader:\n" << buffer << "\n";

	buffer[0] = 0;
	glGetShaderInfoLog(fragmentShader, sizeof(buffer), NULL, buffer);
	if (*buffer) error << "Fragment shader:\n" << buffer << "\n";

	buffer[0] = 0;
	glGetProgramInfoLog(program, sizeof(buffer), NULL, buffer);
	if (*buffer) error << "Linking:\n" << buffer << "\n";

	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	// The linked program keeps all it needs: the shader objects would only hold on to their
	// source and their compiled form (under ANGLE a copy of the SPIR-V) as long as the program lives.
	glDetachShader(program, vertexShader);
	glDetachShader(program, fragmentShader);
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);
	return status == GL_TRUE;
}

// GPU instancing (hw_batching.h): GLSL ES 3.00 only, emitted when the flavour asks for it.
static const char *InstancingVertexDecl = R"(
#ifdef INSTANCING
in vec4 aInstXform0;
in vec4 aInstXform1;
in vec4 aInstXform2;
in vec4 aInstXform3;
in vec4 aInstLightAttr;
in vec4 aInstDynLight;
in highp uvec4 aInstColors;
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

bool FShader::Configure(const char* name, const char* vert_prog_lump, const char* fragprog, const char* fragprog2, const char* light_fragprog, const char* defines)
{
	mVertProg = vert_prog_lump;
	mFragProg = fragprog;
	mFragProg2 = fragprog2;
	mLightProg = light_fragprog;
	mDefinesBase = defines;

	return true;
}

void FShader::LoadVariant()
{
	//mDefinesBase
	Load(mName.GetChars(), mVertProg.GetChars(), mFragProg.GetChars(), mFragProg2.GetChars(), mLightProg.GetChars(), mDefinesBase.GetChars());
}

bool FShader::Load(const char * name, const char * vert_prog_lump_, const char * frag_prog_lump_, const char * proc_prog_lump_, const char * light_fragprog_, const char * defines)
{
	ShaderVariantData* shaderData = new ShaderVariantData();

	FString vert_prog_lump = vert_prog_lump_;
	FString frag_prog_lump = frag_prog_lump_;
	FString	proc_prog_lump = proc_prog_lump_;
	FString light_fragprog = light_fragprog_;

	vert_prog_lump.Substitute("shaders/", "shaders_gles/");
	frag_prog_lump.Substitute("shaders/", "shaders_gles/");
	if (!mUserShader) proc_prog_lump.Substitute("shaders/", "shaders_gles/");	// a mod's shader is the mod's file
	light_fragprog.Substitute("shaders/", "shaders_gles/");

	//light_fragprog.Substitute("material_pbr", "material_normal");

	// The light model: GLSL ES 3.00 lights specular and PBR materials by their own models, as the GL
	// renderer does; everything else (and everything with GLSL ES 1.00) by the normal one.
	if (light_fragprog.Len())
	{
		const bool ownModel = gles.essl300 && (light_fragprog.IndexOf("material_pbr") >= 0 || light_fragprog.IndexOf("material_specular") >= 0);
		if (!ownModel)
			light_fragprog = "shaders_gles/glsl/material_normal.fp";
	}


	FString error;

	FString i_data = GetGLSLPrecision();

	i_data += R"(

		// light buffers
		uniform vec4 lights[MAXIMUM_LIGHT_VECTORS];

		// bone matrix buffers
		uniform mat4 bones[MAXIMUM_LIGHT_VECTORS];

		uniform	mat4 ProjectionMatrix;
		uniform	mat4 ViewMatrix;
		uniform	mat4 NormalViewMatrix;

		uniform	vec4 uCameraPos;
		uniform	vec4 uClipLine;

		uniform	float uGlobVis;			// uGlobVis = R_GetGlobVis(r_visibility) / 32.0
		uniform	int uPalLightLevels;
		uniform	int uViewHeight;		// Software fuzz scaling
		uniform	float uClipHeight;
		uniform	float uClipHeightDirection;
		uniform	int uShadowmapFilter;

		uniform float uThickFogDistance;
		uniform float uThickFogMultiplier;

		uniform int uTextureMode;
		uniform vec2 uClipSplit;
		uniform float uAlphaThreshold;

		// colors
		uniform vec4 uObjectColor;
		uniform vec4 uObjectColor2;
		uniform vec4 uDynLightColor;
		uniform vec4 uAddColor;
		uniform vec4 uTextureBlendColor;
		uniform vec4 uTextureModulateColor;
		uniform vec4 uTextureAddColor;
		uniform vec4 uFogColor;
		uniform float uDesaturationFactor;
		uniform float uInterpolationFactor;

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
		//uniform int uFogEnabled;

		// dynamic lights
		uniform ivec4 uLightRange;

		// bone animation
		uniform int uBoneIndexBase;

		// Blinn glossiness and specular level
		uniform vec2 uSpecularMaterial;

		// matrices
		uniform mat4 ModelMatrix;
		uniform mat4 NormalModelMatrix;
		uniform mat4 TextureMatrix;

		uniform vec4 uFixedColormapStart;
		uniform vec4 uFixedColormapRange;

		// textures
		uniform sampler2D tex;
		uniform sampler2D ShadowMap;
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
		uniform sampler2D texture12;	// the last unit a mod's shader can have a texture of its own on, as with GL

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

#ifdef NPOT_EMULATION
	i_data += "#define NPOT_EMULATION\nuniform vec2 uNpotEmulation;\n";
#endif

	int vp_lump = fileSystem.CheckNumForFullName(vert_prog_lump.GetChars(), 0);
	if (vp_lump == -1) I_Error("Unable to load '%s'", vert_prog_lump.GetChars());

	int fp_lump = fileSystem.CheckNumForFullName(frag_prog_lump.GetChars(), 0);
	if (fp_lump == -1) I_Error("Unable to load '%s'", frag_prog_lump.GetChars());



//
// The following code uses GetChars on the strings to get rid of terminating 0 characters. Do not remove or the code may break!
//
	FString vp_comb;

	assert(screen->mLights != NULL);
	assert(screen->mBones != NULL);

	unsigned int lightbuffersize = screen->mLights->GetBlockSize();

	vp_comb.Format("#version %s\n\n#define NO_CLIPDISTANCE_SUPPORT\n", gles.shaderVersionString);

	FString fp_comb = vp_comb;
	vp_comb << defines << i_data.GetChars() << InstancingVertexDecl;
	fp_comb << "$placeholder$\n" << defines << i_data.GetChars() << InstancingFragmentDecl;

	vp_comb << "#line 1\n";
	fp_comb << "#line 1\n";

#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
	if(gles.essl300)
	{
		FString vpcode = GetStringFromLump(vp_lump);
		FString newvpcode = RemoveLayoutLocationDecl(vpcode, "out");
		FString vppatch300 = GenGLSL300PatchCode(0);
		vp_comb << vppatch300;
		FString vpcode300 = GLSL100_to_GLSL300(newvpcode, 0);
		vp_comb << vpcode300;

		FString fpcode = GetStringFromLump(fp_lump);
		FString newfpcode = RemoveLayoutLocationDecl(fpcode, "in");
		FString fppatch300 = GenGLSL300PatchCode(1);
		fp_comb << fppatch300;
		FString fpcode300 = GLSL100_to_GLSL300(newfpcode, 1);
		fp_comb << fpcode300;
	}
	else
	{
#endif
	vp_comb << RemoveLayoutLocationDecl(GetStringFromLump(vp_lump), "out").GetChars() << "\n";
	fp_comb << RemoveLayoutLocationDecl(GetStringFromLump(fp_lump), "in").GetChars() << "\n";
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
	}
#endif
	FString placeholder = "\n";

	if (proc_prog_lump.Len())
	{
		fp_comb << "#line 1\n";

		if (proc_prog_lump[0] != '#')
		{
			int pp_lump = fileSystem.CheckNumForFullName(proc_prog_lump.GetChars());
			if (pp_lump == -1) I_Error("Unable to load '%s'", proc_prog_lump.GetChars());
			FString pp_data = GetStringFromLump(pp_lump);

			if (mUserShader)
			{
				// '#include' in a mod's shader, as with the GL renderer
				TArray<FString> includedFiles;
				FString includeError;
				pp_data = stb_include_string(pp_data, proc_prog_lump, includedFiles, includeError);
				if (includeError.IsNotEmpty()) I_Error("Unable to load '%s': %s", proc_prog_lump.GetChars(), includeError.GetChars());
			}

			if (pp_data.IndexOf("ProcessMaterial") < 0 && pp_data.IndexOf("SetupMaterial") < 0)
			{
				// this looks like an old custom hardware shader.

				if (pp_data.IndexOf("GetTexCoord") >= 0)
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders_gles/glsl/func_defaultmat2.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders_gles/glsl/func_defaultmat2.fp");
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
					if(gles.essl300)
					{
						fp_comb << "\n" << GLSL100_to_GLSL300(GetStringFromLump(pl_lump), 1);
					}
					else
#endif
					fp_comb << "\n" << GetStringFromLump(pl_lump);
				}
				else
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders_gles/glsl/func_defaultmat.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders_gles/glsl/func_defaultmat.fp");
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
					if(gles.essl300)
					{
						fp_comb << "\n" << GLSL100_to_GLSL300(GetStringFromLump(pl_lump), 1);
					}
					else
#endif
					fp_comb << "\n" << GetStringFromLump(pl_lump);

					if (pp_data.IndexOf("ProcessTexel") < 0)
					{
						// this looks like an even older custom hardware shader.
						// We need to replace the ProcessTexel call to make it work.

						fp_comb.Substitute("material.Base = ProcessTexel();", "material.Base = Process(vec4(1.0));");
					}
				}

				if (pp_data.IndexOf("ProcessLight") >= 0)
				{
					// The ProcessLight signatured changed. Forward to the old one.
					fp_comb << "\nvec4 ProcessLight(vec4 color);\n";
					fp_comb << "\nvec4 ProcessLight(Material material, vec4 color) { return ProcessLight(color); }\n";
				}
			}

#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
			if(gles.essl300)
			{
				fp_comb << GLSL100_to_GLSL300(RemoveLegacyUserUniforms(pp_data).GetChars(), 1);
			}
			else
#endif
			fp_comb << RemoveLegacyUserUniforms(pp_data).GetChars();
			fp_comb.Substitute("gl_TexCoord[0]", "vTexCoord");	// fix old custom shaders.

			if (pp_data.IndexOf("ProcessLight") < 0)
			{
				int pl_lump = fileSystem.CheckNumForFullName("shaders_gles/glsl/func_defaultlight.fp", 0);
				if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders_gles/glsl/func_defaultlight.fp");
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
				if(gles.essl300)
				{
					fp_comb << "\n" << GLSL100_to_GLSL300(GetStringFromLump(pl_lump), 1);
				}
				else
#endif
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
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
			if(gles.essl300)
			{
				fp_comb << GLSL100_to_GLSL300(proc_prog_lump.GetChars() + 1, 1);
			}
			else
#endif
			fp_comb << proc_prog_lump.GetChars() + 1;
		}
	}
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
	if(gles.essl300)
	{
		fp_comb << GLSL100_to_GLSL300(placeholder, 1);
	}
#endif
	fp_comb.Substitute("$placeholder$", placeholder);

	if (light_fragprog.Len())
	{
		int pp_lump = fileSystem.CheckNumForFullName(light_fragprog.GetChars(), 0);
		if (pp_lump == -1) I_Error("Unable to load '%s'", light_fragprog.GetChars());
		fp_comb << GetStringFromLump(pp_lump) << "\n";
	}

	if (gles.flags & RFL_NO_CLIP_PLANES)
	{
		// On ATI's GL3 drivers we have to disable gl_ClipDistance because it's hopelessly broken.
		// This will cause some glitches and regressions but is the only way to avoid total display garbage.
		vp_comb.Substitute("gl_ClipDistance", "//");
	}

	shaderData->hShader = glCreateProgram();

	uint32_t binaryFormat = 0;
	TArray<uint8_t> binary;
	if (IsShaderCacheActive())
		binary = LoadCachedProgramBinary(vp_comb, fp_comb, binaryFormat);

	bool linked = false;

	if (binary.Size() > 0 && glProgramBinary)
	{
		glProgramBinary(shaderData->hShader, binaryFormat, binary.Data(), binary.Size());
		GLint status = 0;
		glGetProgramiv(shaderData->hShader, GL_LINK_STATUS, &status);
		linked = (status == GL_TRUE);
		if (!linked)
			RejectCachedProgramBinary(vp_comb, fp_comb);
	}

	if (!linked && sCacheOnly)
	{
		// only looked up in the binary cache (variant prewarm): not there, nothing is compiled now
		glDeleteProgram(shaderData->hShader);
		delete shaderData;
		return false;
	}

	if (!linked)
	{
		linked = BuildProgram(shaderData->hShader, vp_comb, fp_comb, error);

#ifdef ANDROID
		// A mod's shader written for the GL renderer's desktop GLSL (implicit conversions and the
		// like) that the driver refuses as GLSL ES 3.00: GLSL -> SPIR-V -> GLSL ES 3.00 by NG-GL4ES,
		// as the GL renderer builds all of its shaders. The vertex shader is the built-in one.
		if (!linked && mUserShader && UseMaterialShaderConverter())
		{
			const std::string converted = OpenGLRenderer::ConvertShaderToGLES(fp_comb.GetChars(), false);
			if (converted != fp_comb.GetChars())
			{
				// SPIRV-Cross declares the samplers highp. A uniform in both stages must have the same
				// precision, and the vertex shader's default for samplers is lowp (it only declares them).
				FString vp_highpSamplers = vp_comb;
				const ptrdiff_t versionEnd = vp_highpSamplers.IndexOf('\n');
				vp_highpSamplers.Insert((size_t)(versionEnd + 1), "precision highp sampler2D;\n");

				FString convertedError;
				linked = BuildProgram(shaderData->hShader, vp_highpSamplers, FString(converted.c_str()), convertedError);
				if (!linked) error << "Converted by NG-GL4ES (SPIRV-Cross):\n" << convertedError;
			}
			else
			{
				error << "NG-GL4ES (SPIRV-Cross) could not convert it either\n";
			}
		}
#endif

		if (!linked && mUserShader)
		{
			// The built-in shader of the material type for this one from now on, this variant included
			Printf(TEXTCOLOR_RED "Material shader '%s' cannot be used with OpenGL ES, the built-in shader of its material type is used instead:\n%s\n", name, error.GetChars());
			glDeleteProgram(shaderData->hShader);
			delete shaderData;

			const FString definesBase = mDefinesBase;
			FString variantDefines = defines;
			if (strncmp(variantDefines.GetChars(), definesBase.GetChars(), definesBase.Len()) == 0)
				variantDefines = variantDefines.Mid(definesBase.Len());

			mUserShader = false;
			mFragProg2 = mFallbackFragProg2;
			mDefinesBase = mFallbackDefines;
			return Load(name, vert_prog_lump_, frag_prog_lump_, mFragProg2.GetChars(), light_fragprog_, (mDefinesBase + variantDefines).GetChars());
		}

		if (!linked)
		{
			// only print message if there's an error.
			I_Error("Init Shader '%s':\n%s\n", name, error.GetChars());
		}
		else if (glProgramBinary && IsShaderCacheActive())
		{
			int binaryLength = 0;
			glGetProgramiv(shaderData->hShader, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
			binary.Resize(binaryLength);
			if (binaryLength > 0)
			{
				binaryLength = 0;
				glGetProgramBinary(shaderData->hShader, binary.Size(), &binaryLength, &binaryFormat, binary.Data());
				binary.Resize(binaryLength > 0 ? binaryLength : 0);
				SaveCachedProgramBinary(vp_comb, fp_comb, binary, binaryFormat);
			}
		}
	}
	shaderData->hVertProg = 0;
	shaderData->hFragProg = 0;

	shaderData->muProjectionMatrix.Init(shaderData->hShader, "ProjectionMatrix");
	shaderData->muViewMatrix.Init(shaderData->hShader, "ViewMatrix");
	shaderData->muNormalViewMatrix.Init(shaderData->hShader, "NormalViewMatrix");

	shaderData->muCameraPos.Init(shaderData->hShader, "uCameraPos");
	shaderData->muClipLine.Init(shaderData->hShader, "uClipLine");

	shaderData->muGlobVis.Init(shaderData->hShader, "uGlobVis");
	shaderData->muPalLightLevels.Init(shaderData->hShader, "uPalLightLevels");
	shaderData->muViewHeight.Init(shaderData->hShader, "uViewHeight");
	shaderData->muClipHeight.Init(shaderData->hShader, "uClipHeight");
	shaderData->muClipHeightDirection.Init(shaderData->hShader, "uClipHeightDirection");
	shaderData->muShadowmapFilter.Init(shaderData->hShader, "uShadowmapFilter");

	shaderData->muThickFogDistance.Init(shaderData->hShader, "uThickFogDistance");
	shaderData->muThickFogMultiplier.Init(shaderData->hShader, "uThickFogMultiplier");
	////

	shaderData->muDesaturation.Init(shaderData->hShader, "uDesaturationFactor");
	shaderData->muFogEnabled.Init(shaderData->hShader, "uFogEnabled");
	shaderData->muTextureMode.Init(shaderData->hShader, "uTextureMode");
	shaderData->muLightParms.Init(shaderData->hShader, "uLightAttr");
	shaderData->muClipSplit.Init(shaderData->hShader, "uClipSplit");
	shaderData->muLightRange.Init(shaderData->hShader, "uLightRange");
	shaderData->muBoneIndexBase.Init(shaderData->hShader, "uBoneIndexBase");
	shaderData->muFogColor.Init(shaderData->hShader, "uFogColor");
	shaderData->muDynLightColor.Init(shaderData->hShader, "uDynLightColor");
	shaderData->muObjectColor.Init(shaderData->hShader, "uObjectColor");
	shaderData->muObjectColor2.Init(shaderData->hShader, "uObjectColor2");
	shaderData->muGlowBottomColor.Init(shaderData->hShader, "uGlowBottomColor");
	shaderData->muGlowTopColor.Init(shaderData->hShader, "uGlowTopColor");
	shaderData->muGlowBottomPlane.Init(shaderData->hShader, "uGlowBottomPlane");
	shaderData->muGlowTopPlane.Init(shaderData->hShader, "uGlowTopPlane");
	shaderData->muGradientBottomPlane.Init(shaderData->hShader, "uGradientBottomPlane");
	shaderData->muGradientTopPlane.Init(shaderData->hShader, "uGradientTopPlane");
	shaderData->muSplitBottomPlane.Init(shaderData->hShader, "uSplitBottomPlane");
	shaderData->muSplitTopPlane.Init(shaderData->hShader, "uSplitTopPlane");
	shaderData->muDetailParms.Init(shaderData->hShader, "uDetailParms");
	shaderData->muInterpolationFactor.Init(shaderData->hShader, "uInterpolationFactor");
	shaderData->muAlphaThreshold.Init(shaderData->hShader, "uAlphaThreshold");
	shaderData->muSpecularMaterial.Init(shaderData->hShader, "uSpecularMaterial");
	shaderData->muAddColor.Init(shaderData->hShader, "uAddColor");
	shaderData->muTextureAddColor.Init(shaderData->hShader, "uTextureAddColor");
	shaderData->muTextureModulateColor.Init(shaderData->hShader, "uTextureModulateColor");
	shaderData->muTextureBlendColor.Init(shaderData->hShader, "uTextureBlendColor");
	shaderData->muTimer.Init(shaderData->hShader, "timer");
#ifdef NPOT_EMULATION
	shaderData->muNpotEmulation.Init(shaderData->hShader, "uNpotEmulation");
#endif
	shaderData->muFixedColormapStart.Init(shaderData->hShader, "uFixedColormapStart");
	shaderData->muFixedColormapRange.Init(shaderData->hShader, "uFixedColormapRange");

	shaderData->lights_index = glGetUniformLocation(shaderData->hShader, "lights");
	shaderData->bones_index = glGetUniformLocation(shaderData->hShader, "bones");
	shaderData->modelmatrix_index = glGetUniformLocation(shaderData->hShader, "ModelMatrix");
	shaderData->texturematrix_index = glGetUniformLocation(shaderData->hShader, "TextureMatrix");
	shaderData->normalmodelmatrix_index = glGetUniformLocation(shaderData->hShader, "NormalModelMatrix");


	glUseProgram(shaderData->hShader);

	// set up other texture units (if needed by the shader)
	for (int i = 2; i<16; i++)
	{
		char stringbuf[20];
		mysnprintf(stringbuf, 20, "texture%d", i);
		int tempindex = glGetUniformLocation(shaderData->hShader, stringbuf);
		if (tempindex >= 0) glUniform1i(tempindex, i - 1);
	}

	int shadowmapindex = glGetUniformLocation(shaderData->hShader, "ShadowMap");
	if (shadowmapindex >= 0) glUniform1i(shadowmapindex, 16);

	glUseProgram(0);

	cur = shaderData;

	return linked;
}

//==========================================================================
//
//
//
//==========================================================================

FShader::~FShader()
{
	std::map<uint32_t, ShaderVariantData*>::iterator it = variants.begin();

	while (it != variants.end())
	{
		glDeleteProgram(it->second->hShader);
		if (it->second->hVertProg != 0)
			glDeleteShader(it->second->hVertProg);
		if (it->second->hFragProg != 0)
			glDeleteShader(it->second->hFragProg);
		it++;
	}
}


//==========================================================================
//
//
//
//==========================================================================

bool FShader::sCacheOnly = false;
bool FShader::sStartupDone = false;

static FString VariantConfig(const ShaderFlavourData &flavour)
{
	FString variantConfig = "\n";

	variantConfig.AppendFormat("#define MAXIMUM_LIGHT_VECTORS %d\n", gles.numlightvectors);
	variantConfig.AppendFormat("#define DEF_TEXTURE_MODE %d\n", flavour.textureMode);
	variantConfig.AppendFormat("#define DEF_TEXTURE_FLAGS %d\n", flavour.texFlags);
	variantConfig.AppendFormat("#define DEF_BLEND_FLAGS %d\n", flavour.blendFlags & 0x7);
	variantConfig.AppendFormat("#define DEF_FOG_2D %d\n", flavour.twoDFog);
	variantConfig.AppendFormat("#define DEF_FOG_ENABLED %d\n", flavour.fogEnabled);
	variantConfig.AppendFormat("#define DEF_FOG_RADIAL %d\n", flavour.fogEquationRadial);
	variantConfig.AppendFormat("#define DEF_FOG_COLOURED %d\n", flavour.colouredFog);
	variantConfig.AppendFormat("#define DEF_USE_U_LIGHT_LEVEL %d\n", flavour.useULightLevel);

	variantConfig.AppendFormat("#define DEF_DO_DESATURATE %d\n", flavour.doDesaturate);

	variantConfig.AppendFormat("#define DEF_DYNAMIC_LIGHTS_MOD %d\n", flavour.dynLightsMod);
	variantConfig.AppendFormat("#define DEF_DYNAMIC_LIGHTS_SUB %d\n", flavour.dynLightsSub);
	variantConfig.AppendFormat("#define DEF_DYNAMIC_LIGHTS_ADD %d\n", flavour.dynLightsAdd);

	variantConfig.AppendFormat("#define DEF_USE_OBJECT_COLOR_2 %d\n", flavour.useObjectColor2);
	variantConfig.AppendFormat("#define DEF_USE_GLOW_TOP_COLOR %d\n", flavour.useGlowTopColor);
	variantConfig.AppendFormat("#define DEF_USE_GLOW_BOTTOM_COLOR %d\n", flavour.useGlowBottomColor);

	variantConfig.AppendFormat("#define DEF_USE_COLOR_MAP %d\n", flavour.useColorMap);
	variantConfig.AppendFormat("#define DEF_BUILD_LIGHTING %d\n", flavour.buildLighting);
	variantConfig.AppendFormat("#define DEF_BANDED_SW_LIGHTING %d\n", flavour.bandedSwLight);

	variantConfig.AppendFormat("#define USE_GLSL_V100 %d\n", gles.forceGLSLv100);

#ifdef NPOT_EMULATION
	variantConfig.AppendFormat("#define DEF_NPOT_EMULATION %d\n", flavour.npotEmulation);
#endif

	variantConfig.AppendFormat("#define DEF_HAS_SPOTLIGHT %d\n", flavour.hasSpotLight);
	variantConfig.AppendFormat("#define DEF_PALETTE_INTERPOLATE %d\n", flavour.paletteInterpolate);
	variantConfig.AppendFormat("#define DEF_LIGHT_BLEND_MODE %d\n", flavour.lightBlendMode);
	variantConfig.AppendFormat("#define DEF_VANILLA_LIGHTING %d\n", flavour.vanillaLighting);
	if (flavour.instancing && gles.essl300)
		variantConfig << "#define INSTANCING\n" << (flavour.instancing == 1 ? "#define INSTANCED_QUADS\n" : (flavour.instancing == 2 ? "#define INSTANCED_MODELS\n" : "#define INSTANCED_WALLS\n"));
	return variantConfig;
}

// A flavour as it is written to the list of the variants used (variants.txt), every field in full
static const int FlavourFieldCount = 24;

static void FlavourToInts(const ShaderFlavourData &f, int *v)
{
	int i = 0;
	v[i++] = f.textureMode; v[i++] = f.texFlags; v[i++] = f.blendFlags; v[i++] = f.twoDFog;
	v[i++] = f.fogEnabled; v[i++] = f.fogEquationRadial; v[i++] = f.colouredFog; v[i++] = f.doDesaturate;
	v[i++] = f.dynLightsMod; v[i++] = f.dynLightsSub; v[i++] = f.dynLightsAdd; v[i++] = f.useULightLevel;
	v[i++] = f.useObjectColor2; v[i++] = f.useGlowTopColor; v[i++] = f.useGlowBottomColor; v[i++] = f.useColorMap;
	v[i++] = f.buildLighting; v[i++] = f.bandedSwLight;
#ifdef NPOT_EMULATION
	v[i++] = f.npotEmulation;
#else
	v[i++] = 0;
#endif
	v[i++] = f.hasSpotLight; v[i++] = f.paletteInterpolate; v[i++] = f.instancing;
	v[i++] = f.lightBlendMode; v[i++] = f.vanillaLighting;
	assert(i == FlavourFieldCount);
}

static void FlavourFromInts(const int *v, ShaderFlavourData &f)
{
	int i = 0;
	f.textureMode = v[i++]; f.texFlags = v[i++]; f.blendFlags = v[i++]; f.twoDFog = v[i++] != 0;
	f.fogEnabled = v[i++] != 0; f.fogEquationRadial = v[i++] != 0; f.colouredFog = v[i++] != 0; f.doDesaturate = v[i++] != 0;
	f.dynLightsMod = v[i++] != 0; f.dynLightsSub = v[i++] != 0; f.dynLightsAdd = v[i++] != 0; f.useULightLevel = v[i++] != 0;
	f.useObjectColor2 = v[i++] != 0; f.useGlowTopColor = v[i++] != 0; f.useGlowBottomColor = v[i++] != 0; f.useColorMap = v[i++] != 0;
	f.buildLighting = v[i++] != 0; f.bandedSwLight = v[i++] != 0;
#ifdef NPOT_EMULATION
	f.npotEmulation = v[i++] != 0;
#else
	i++;
#endif
	f.hasSpotLight = v[i++] != 0; f.paletteInterpolate = v[i++] != 0; f.instancing = v[i++];
	f.lightBlendMode = v[i++]; f.vanillaLighting = v[i++] != 0;
	assert(i == FlavourFieldCount);
}

// "p<pass>|<hash of the sources and base defines>|<name>|": the shader a recorded flavour belongs to
// (a material shader with and without alpha test has the same name, other defines)
FString FShader::VariantPrefix() const
{
	uint32_t hash = 2166136261u;
	for (const FString *s : { &mVertProg, &mFragProg, &mFragProg2, &mLightProg, &mDefinesBase })
	{
		for (const char *c = s->GetChars(); c && *c; c++)
			hash = (hash ^ uint8_t(*c)) * 16777619u;
		hash = (hash ^ 0xffu) * 16777619u;	// keeps ("ab","c") and ("a","bc") apart
	}
	FString id;
	id.Format("p%d|%08x|%s|", mPassType, hash, mName.GetChars());
	return id;
}

FShader::ShaderVariantData *FShader::BuildVariant(const ShaderFlavourData &flavour)
{
	if (!Load(mName.GetChars(), mVertProg.GetChars(), mFragProg.GetChars(), mFragProg2.GetChars(), mLightProg.GetChars(), (mDefinesBase + VariantConfig(flavour)).GetChars()))
		return nullptr;	// sCacheOnly: not in the cache (cur unchanged)
	return cur;
}

void FShader::RecordVariantUse(const ShaderFlavourData &flavour) const
{
	if (!IsShaderCacheActive())
		return;
	int v[FlavourFieldCount];
	FlavourToInts(flavour, v);
	FString line = VariantPrefix();
	for (int i = 0; i < FlavourFieldCount; i++)
		line.AppendFormat(i ? " %d" : "%d", v[i]);
	ProgramBinaryCache::RecordVariant(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars(), line);
}

int FShader::PrewarmVariants(const TArray<FString> &recorded)
{
	const FString prefix = VariantPrefix();
	int loaded = 0;
	for (auto &line : recorded)
	{
		if (line.Len() <= prefix.Len() || strncmp(line.GetChars(), prefix.GetChars(), prefix.Len()) != 0)
			continue;
		int v[FlavourFieldCount];
		int n = 0;
		const char *p = line.GetChars() + prefix.Len();
		while (n < FlavourFieldCount)
		{
			char *end = nullptr;
			const long value = strtol(p, &end, 10);
			if (end == p)
				break;
			v[n++] = (int)value;
			p = end;
		}
		if (n != FlavourFieldCount || *p != 0)
			continue;	// written by another build
		ShaderFlavourData flavour;
		FlavourFromInts(v, flavour);
		const uint32_t tag = CreateShaderTag(flavour);
		if (variants.find(tag) != variants.end())
			continue;
		if (ShaderVariantData *data = BuildVariant(flavour))
		{
			variants.insert(std::make_pair(tag, data));
			loaded++;
		}
	}
	return loaded;
}

bool FShader::Bind(ShaderFlavourData& flavour)
{
	uint32_t tag = CreateShaderTag(flavour);

	auto pos = variants.find(tag);

	if (pos == variants.end())
	{
		ShaderVariantData *data = BuildVariant(flavour);
		variants.insert(std::make_pair(tag, data));
		cur = data;
		RecordVariantUse(flavour);
	}
	else
	{
		cur = pos->second;
	}

	GLRenderer->mShaderManager->SetActiveShader(this->cur);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FShader *FShaderCollection::Compile (const char *ShaderName, const char *ShaderPath, const char *LightModePath, const char *shaderdefines, bool usediscard, EPassType passType)
{
	FString defines;
	defines += shaderdefines;
	// this can't be in the shader code due to ATI strangeness.
	if (!usediscard) defines += "#define NO_ALPHATEST\n";

	FShader *shader = new FShader(ShaderName);
	shader->mPassType = passType;
	shader->Configure(ShaderName, "shaders_gles/glsl/main.vp", "shaders_gles/glsl/main.fp", ShaderPath, LightModePath, defines.GetChars());
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

FShaderManager::~FShaderManager()
{
	glUseProgram(0);
	mActiveShader = NULL;

	for (auto collection : mPassShaders)
		delete collection;
}

void FShaderManager::SetActiveShader(FShader::ShaderVariantData *sh)
{
	if (mActiveShader != sh)
	{
		glUseProgram(sh!= NULL? sh->GetHandle() : 0);
		mActiveShader = sh;
	}
}

void FShaderManager::PrewarmVariants()
{
	if (!IsShaderCacheActive())
	{
		FShader::sStartupDone = true;
		return;
	}
	const TArray<FString> recorded = ProgramBinaryCache::RecordedVariants(ProgramCacheNamespace().GetChars(), ProgramCacheDriver().GetChars());
	FShader::sCacheOnly = true;
	for (auto collection : mPassShaders)
		collection->PrewarmVariants(recorded);
	FShader::sCacheOnly = false;
	FShader::sStartupDone = true;
	glUseProgram(0);
	mActiveShader = nullptr;
}

FShader *FShaderManager::BindEffect(int effect, EPassType passType, ShaderFlavourData &flavour)
{
	if (passType < mPassShaders.Size())
		return mPassShaders[passType]->BindEffect(effect, flavour);
	else
		return nullptr;
}

FShader *FShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (passType < mPassShaders.Size())
		return mPassShaders[passType]->Get(eff, alphateston);
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
	CompileShaders(passType);
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

void FShaderCollection::CompileShaders(EPassType passType)
{
	mMaterialShaders.Clear();
	mMaterialShadersNAT.Clear();
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		mEffectShaders[i] = NULL;
	}

	for(int i=0;defaultshaders[i].ShaderName != NULL;i++)
	{
		FShader *shc = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, true, passType);
		mMaterialShaders.Push(shc);
		if (i < SHADER_NoTexture)
		{
			shc = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, false, passType);
			mMaterialShadersNAT.Push(shc);
		}
	}

	// Mods' material shaders (GLDEFS): GLES 3.0 mode only (gl_customshader is off otherwise, see
	// gles_system.cpp). The indices from FIRST_USER_SHADER on are theirs, as with GL.
	if (gles.essl300 && gl_customshader)
	{
		for(unsigned i = 0; i < usershaders.Size(); i++)
		{
			const int shaderType = usershaders[i].shaderType;
			FString name = ExtractFileBase(usershaders[i].shader.GetChars());
			// USER_SHADER: main.vp/main.fp keep every vertex output (custom code may read any of them)
			// and main.fp has the interface of the GL renderer's main.fp that mods' shaders use.
			FString defines = FString("#define USER_SHADER\n") + defaultshaders[shaderType].Defines + usershaders[i].defines;
			FShader *shc = Compile(name.GetChars(), usershaders[i].shader.GetChars(), defaultshaders[shaderType].lightfunc, defines.GetChars(), true, passType);
			shc->mUserShader = true;
			shc->mFallbackFragProg2 = defaultshaders[shaderType].gettexelfunc;
			shc->mFallbackDefines = defaultshaders[shaderType].Defines;
			mMaterialShaders.Push(shc);
		}
	}

	for(int i=0;i<MAX_EFFECTS;i++)
	{
		FShader *eff = new FShader(effectshaders[i].ShaderName);
		eff->mPassType = passType;
		if (!eff->Configure(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1,
						effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines))
		{
			delete eff;
		}
		else mEffectShaders[i] = eff;
	}
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

int FShaderCollection::PrewarmVariants(const TArray<FString> &recorded)
{
	int loaded = 0;
	for (auto shader : mMaterialShaders)
		if (shader) loaded += shader->PrewarmVariants(recorded);
	for (auto shader : mMaterialShadersNAT)
		if (shader) loaded += shader->PrewarmVariants(recorded);
	for (auto shader : mEffectShaders)
		if (shader) loaded += shader->PrewarmVariants(recorded);
	return loaded;
}

FShader *FShaderCollection::BindEffect(int effect, ShaderFlavourData& flavour)
{
	if (effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[effect] != NULL)
	{
		mEffectShaders[effect]->Bind(flavour);
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
