/*
** gles_shaderprogram.cpp
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "gles_system.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "gles_shaderprogram.h"
#include "hw_shaderpatcher.h"
#include "filesystem.h"
#include "printf.h"
#include "cmdlib.h"
#include <string>

#ifdef ANDROID
namespace OpenGLRenderer
{
	// gl_shader.cpp: NG-GL4ES's GLSL -> GLSL ES converter, shared with the GL renderer
	bool TryLoadShaderConverter();
	std::string ConvertShaderToGLES(const char *shaderSource, bool isVertexShader);
}
#endif

namespace OpenGLESRenderer
{

// Post-process shaders go through NG-GL4ES when it can be loaded (checked once).
static bool UseShaderConverter()
{
#ifdef ANDROID
	static int available = -1;
	if (available < 0)
	{
		available = OpenGLRenderer::TryLoadShaderConverter() ? 1 : 0;
		Printf("GLES post-process shaders: %s\n", available ? "converted by NG-GL4ES (SPIRV-Cross)" : "NG-GL4ES not available, patched to GLSL ES 3.00");
	}
	return available == 1;
#else
	return false;
#endif
}

FString GetGLSLPrecision();

bool IsShaderCacheActive();
TArray<uint8_t> LoadCachedProgramBinary(const FString &vertex, const FString &fragment, uint32_t &binaryFormat);
void SaveCachedProgramBinary(const FString &vertex, const FString &fragment, const TArray<uint8_t> &binary, uint32_t binaryFormat);
void RejectCachedProgramBinary(const FString &vertex, const FString &fragment);

FShaderProgram::FShaderProgram()
{
	for (int i = 0; i < NumShaderTypes; i++)
		mShaders[i] = 0;
}

//==========================================================================
//
// Free shader program resources
//
//==========================================================================

FShaderProgram::~FShaderProgram()
{
	if (mProgram != 0)
		glDeleteProgram(mProgram);

	for (int i = 0; i < NumShaderTypes; i++)
	{
		if (mShaders[i] != 0)
			glDeleteShader(mShaders[i]);
	}
}

//==========================================================================
//
// Creates an OpenGL shader object for the specified type of shader
//
//==========================================================================

void FShaderProgram::CreateShader(ShaderType type)
{
	GLenum gltype = 0;
	switch (type)
	{
	default:
	case Vertex: gltype = GL_VERTEX_SHADER; break;
	case Fragment: gltype = GL_FRAGMENT_SHADER; break;
	}
	mShaders[type] = glCreateShader(gltype);
}

//==========================================================================
//
// Compiles a shader and attaches it the program object
//
//==========================================================================

void FShaderProgram::Compile(ShaderType type, const char *lumpName, const char *defines, int maxGlslVersion)
{
	int lump = fileSystem.CheckNumForFullName(lumpName);
	if (lump == -1) I_FatalError("Unable to load '%s'", lumpName);
	FString code = GetStringFromLump(lump);
	Compile(type, lumpName, code, defines, maxGlslVersion);
}

void FShaderProgram::Compile(ShaderType type, const char *name, const FString &code, const char *defines, int maxGlslVersion)
{
	mShaderNames[type] = name;
	if (mDesktopGLSL)
	{
		// The source to convert is what the program binary cache knows the program by. The
		// patched GLSL ES version is kept in case the converter or the driver refuses it.
		TArray<std::pair<FString, int>> fallbackBindings;
		const bool converter = UseShaderConverter();
		mShaderSources[type] = PatchDesktopShader(type, code, defines, maxGlslVersion, converter, samplerstobind);
		mFallbackSources[type] = converter ? PatchDesktopShader(type, code, defines, maxGlslVersion, false, fallbackBindings) : FString();
	}
	else
	{
		mShaderSources[type] = PatchShader(type, code, defines, maxGlslVersion);
	}
}

bool FShaderProgram::CompileSource(GLuint handle, const FString &source)
{
	int lengths[1] = { (int)source.Len() };
	const char *sources[1] = { source.GetChars() };
	glShaderSource(handle, 1, sources, lengths);
	glCompileShader(handle);

	GLint status = 0;
	glGetShaderiv(handle, GL_COMPILE_STATUS, &status);
	return status != GL_FALSE;
}

void FShaderProgram::CompileShader(ShaderType type)
{
	CreateShader(type);

	const auto &handle = mShaders[type];

	FString source = mShaderSources[type];
#ifdef ANDROID
	if (mDesktopGLSL && mFallbackSources[type].IsNotEmpty())
	{
		// GLSL -> SPIR-V -> GLSL ES 3.00. The converter hands the source back unchanged when it fails.
		const std::string converted = OpenGLRenderer::ConvertShaderToGLES(source.GetChars(), type == Vertex);
		if (converted != source.GetChars())
			source = converted.c_str();
		else
		{
			Printf("NG-GL4ES could not convert '%s', patching it to GLSL ES 3.00 instead\n", mShaderNames[type].GetChars());
			source = mFallbackSources[type];
		}
	}
#endif

	bool compiled = CompileSource(handle, source);
	if (!compiled && mDesktopGLSL && mFallbackSources[type].IsNotEmpty() && source.Compare(mFallbackSources[type]) != 0)
	{
		Printf("Converted shader '%s' does not compile:\n%s\nusing it patched to GLSL ES 3.00 instead\n", mShaderNames[type].GetChars(), GetShaderInfoLog(handle).GetChars());
		compiled = CompileSource(handle, mFallbackSources[type]);
	}

	if (!compiled)
	{
		if (!mDesktopGLSL)
			I_FatalError("Compile Shader '%s':\n%s\n", mShaderNames[type].GetChars(), GetShaderInfoLog(handle).GetChars());

		Printf(TEXTCOLOR_RED "Post-process shader '%s' cannot be used with OpenGL ES, the effect is left out:\n%s\n", mShaderNames[type].GetChars(), GetShaderInfoLog(handle).GetChars());
		mFailed = true;
		return;
	}

	if (mProgram == 0)
		mProgram = glCreateProgram();
	glAttachShader(mProgram, handle);
}

//==========================================================================
//
// Links a program with the compiled shaders
//
//==========================================================================

void FShaderProgram::Link(const char *name)
{

	uint32_t binaryFormat = 0;
	TArray<uint8_t> binary;
	if (IsShaderCacheActive())
		binary = LoadCachedProgramBinary(mShaderSources[Vertex], mShaderSources[Fragment], binaryFormat);

	bool loadedFromBinary = false;

	if (binary.Size() > 0 && glProgramBinary)
	{
		if (mProgram == 0)
			mProgram = glCreateProgram();
		glProgramBinary(mProgram, binaryFormat, binary.Data(), binary.Size());
		GLint status = 0;
		glGetProgramiv(mProgram, GL_LINK_STATUS, &status);
		loadedFromBinary = (status == GL_TRUE);
		if (!loadedFromBinary)
			RejectCachedProgramBinary(mShaderSources[Vertex], mShaderSources[Fragment]);
	}

	if (!loadedFromBinary)
	{
		CompileShader(Vertex);
		if (!mFailed) CompileShader(Fragment);
		if (mFailed) return;

		if (IsShaderCacheActive() && gles.programBinaryHint)	// (not with OES_get_program_binary: binaries are always retrievable)
			glProgramParameteri(mProgram, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glLinkProgram(mProgram);

		GLint status = 0;
		glGetProgramiv(mProgram, GL_LINK_STATUS, &status);
		if (status == GL_FALSE && mDesktopGLSL)
		{
			Printf(TEXTCOLOR_RED "Post-process shader '%s' cannot be used with OpenGL ES, the effect is left out:\n%s\n", name, GetProgramInfoLog(mProgram).GetChars());
			mFailed = true;
			return;
		}
		else if (status == GL_FALSE)
		{
			I_FatalError("Link Shader '%s':\n%s\n", name, GetProgramInfoLog(mProgram).GetChars());
		}
		else if (glProgramBinary && IsShaderCacheActive())
		{
			int binaryLength = 0;
			glGetProgramiv(mProgram, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
			binary.Resize(binaryLength);
			if (binaryLength > 0)
			{
				binaryLength = 0;
				glGetProgramBinary(mProgram, binary.Size(), &binaryLength, &binaryFormat, binary.Data());
				binary.Resize(binaryLength > 0 ? binaryLength : 0);
				SaveCachedProgramBinary(mShaderSources[Vertex], mShaderSources[Fragment], binary, binaryFormat);
			}
		}
		// The linked program keeps all it needs: the shader objects would only hold on to their
		// source and their compiled form (under ANGLE a copy of the SPIR-V) as long as the program lives.
		for (auto &handle : mShaders)
		{
			if (handle == 0) continue;
			glDetachShader(mProgram, handle);
			glDeleteShader(handle);
			handle = 0;
		}
	}

	// This is only for old OpenGL which didn't allow to set the binding from within the shader.
	if (screen->glslversion < 4.20)
	{
		glUseProgram(mProgram);
		for (auto &uni : samplerstobind)
		{
			auto index = glGetUniformLocation(mProgram, uni.first.GetChars());
			if (index >= 0)
			{
				glUniform1i(index, uni.second);
			}
		}
	}
	samplerstobind.Clear();
	samplerstobind.ShrinkToFit();
}

//==========================================================================
//
// Set uniform buffer location (only useful for GL 3.3)
//
//==========================================================================

void FShaderProgram::SetUniformBufferLocation(int index, const char *name)
{

}

//==========================================================================
//
// Makes the shader the active program
//
//==========================================================================

void FShaderProgram::Bind()
{
	glUseProgram(mProgram);
}

//==========================================================================
//
// Returns the shader info log (warnings and compile errors)
//
//==========================================================================

FString FShaderProgram::GetShaderInfoLog(GLuint handle)
{
	static char buffer[10000];
	GLsizei length = 0;
	buffer[0] = 0;
	glGetShaderInfoLog(handle, 10000, &length, buffer);
	return FString(buffer);
}

//==========================================================================
//
// Returns the program info log (warnings and compile errors)
//
//==========================================================================

FString FShaderProgram::GetProgramInfoLog(GLuint handle)
{
	static char buffer[10000];
	GLsizei length = 0;
	buffer[0] = 0;
	glGetProgramInfoLog(handle, 10000, &length, buffer);
	return FString(buffer);
}

//==========================================================================
//
// Patches a shader to be compatible with the version of OpenGL in use
//
//==========================================================================

FString FShaderProgram::PatchShader(ShaderType type, const FString &code, const char *defines, int maxGlslVersion)
{
	FString patchedCode;

	patchedCode.AppendFormat("#version %s\n", gles.shaderVersionString);

	patchedCode += GetGLSLPrecision();


	if (defines)
		patchedCode << defines;


	patchedCode << "#line 1\n";
#ifdef ANDROID // GLES 3.0: convert the GLSL ES 1.00 sources to GLSL ES 3.00
	if(gles.essl300)
	{
		FString newcode = RemoveLayoutLocationDecl(code, type == Vertex ? "out" : "in");
		FString patch300 = GenGLSL300PatchCode((int)type);
		patchedCode << patch300;
		FString code300 = GLSL100_to_GLSL300(newcode, (int)type);
		patchedCode << code300;
	}
	else
#endif
	patchedCode << RemoveLayoutLocationDecl(code, type == Vertex ? "out" : "in");

	if (maxGlslVersion < 420)
	{
		// Here we must strip out all layout(binding) declarations for sampler uniforms and store them in 'samplerstobind' which can then be processed by the link function.
		patchedCode = RemoveSamplerBindings(patchedCode, samplerstobind);
	}

	return patchedCode;
}

//==========================================================================
//
// Post-process shaders: desktop GLSL, as the GL renderer patches it on
// Android - '#version 410' for the converter, '#version 300 es' with default
// precisions when patched directly. Sampler bindings are set at link time
// (GLSL ES 3.00 has no layout(binding)).
//
//==========================================================================

FString FShaderProgram::PatchDesktopShader(ShaderType type, const FString &code, const char *defines, int maxGlslVersion, bool forConverter, TArray<std::pair<FString, int>> &bindings)
{
	FString patchedCode;

	if (forConverter)
	{
		patchedCode << "#version 410\n#define NO_CLIPDISTANCE_SUPPORT\n#define TARGET_ESSL300\n";
	}
	else
	{
		patchedCode << "#version 300 es\n#define NO_CLIPDISTANCE_SUPPORT\n#define TARGET_ESSL300\n";
		patchedCode << "precision highp int;\n";
		patchedCode << "precision highp float;\n";
		patchedCode << "precision highp sampler2D;\n";
		patchedCode << "precision highp sampler2DArray;\n";
		patchedCode << "precision highp samplerCube;\n";
	}

	if (defines)
		patchedCode << defines;

	patchedCode << "precision highp int;\n";
	patchedCode << "precision highp float;\n";

	patchedCode << "#line 1\n";
	patchedCode << RemoveLayoutLocationDecl(code, type == Vertex ? "out" : "in");

	if (maxGlslVersion < 420)
		patchedCode = RemoveSamplerBindings(patchedCode, bindings);

	return patchedCode;
}

void FShaderProgram::SetPPUniforms(const std::vector<UniformFieldDesc> &fields)
{
	mPPUniforms.clear();
	if (mFailed || mProgram == 0) return;

	for (const auto &field : fields)
	{
		const int location = glGetUniformLocation(mProgram, field.Name);
		if (location != -1)
			mPPUniforms.push_back({ field, location });
	}
	mTimeDeltaLoc = glGetUniformLocation(mProgram, "InputTimeDelta");
	mTimeLoc = glGetUniformLocation(mProgram, "InputTime");
	mTimeGameLoc = glGetUniformLocation(mProgram, "InputTimeGame");
}

void FShaderProgram::ApplyPPUniforms(const TArray<uint8_t> &data)
{
	for (const auto &uniform : mPPUniforms)
	{
		const UniformFieldDesc &desc = uniform.first;
		const int location = uniform.second;

		size_t size;
		switch (desc.Type)
		{
		default:
		case UniformType::Int:
		case UniformType::UInt:
		case UniformType::Float: size = 4; break;
		case UniformType::Vec2:
		case UniformType::IVec2:
		case UniformType::UVec2: size = 8; break;
		case UniformType::Vec3:
		case UniformType::IVec3:
		case UniformType::UVec3: size = 12; break;
		case UniformType::Vec4:
		case UniformType::IVec4:
		case UniformType::UVec4: size = 16; break;
		case UniformType::Mat4: size = 64; break;
		}
		if (desc.Offset + size > data.Size())
			continue;

		uint32_t value[16];
		memcpy(value, data.Data() + desc.Offset, size);
		const GLfloat *f = (const GLfloat *)value;
		const GLint *i = (const GLint *)value;
		const GLuint *u = (const GLuint *)value;

		switch (desc.Type)
		{
		default:
		case UniformType::Int: glUniform1iv(location, 1, i); break;
		case UniformType::UInt: glUniform1uiv(location, 1, u); break;
		case UniformType::Float: glUniform1fv(location, 1, f); break;
		case UniformType::Vec2: glUniform2fv(location, 1, f); break;
		case UniformType::Vec3: glUniform3fv(location, 1, f); break;
		case UniformType::Vec4: glUniform4fv(location, 1, f); break;
		case UniformType::IVec2: glUniform2iv(location, 1, i); break;
		case UniformType::IVec3: glUniform3iv(location, 1, i); break;
		case UniformType::IVec4: glUniform4iv(location, 1, i); break;
		case UniformType::UVec2: glUniform2uiv(location, 1, u); break;
		case UniformType::UVec3: glUniform3uiv(location, 1, u); break;
		case UniformType::UVec4: glUniform4uiv(location, 1, u); break;
		case UniformType::Mat4: glUniformMatrix4fv(location, 1, GL_FALSE, f); break;
		}
	}
}

/////////////////////////////////////////////////////////////////////////////

void FPresentShaderBase::Init(const char * vtx_shader_name, const char * program_name)
{
	FString prolog = Uniforms.CreateDeclaration("Uniforms", PresentUniforms::Desc());

	mShader.reset(new FShaderProgram());
	mShader->Compile(FShaderProgram::Vertex, "shaders_gles/pp/screenquad.vp", prolog.GetChars(), 330);
	mShader->Compile(FShaderProgram::Fragment, vtx_shader_name, prolog.GetChars(), 330);
	mShader->Link(program_name);
	mShader->Bind();
	Uniforms.Init();

	Uniforms.UniformLocation.resize(Uniforms.mFields.size());

	for (size_t n = 0; n < Uniforms.mFields.size(); n++)
	{
		int index = -1;
		UniformFieldDesc desc = Uniforms.mFields[n];
		index = glGetUniformLocation(mShader->mProgram, desc.Name);
		Uniforms.UniformLocation[n] = index;
	}
}

void FPresentShader::Bind()
{
	if (!mShader)
	{
		Init("shaders_gles/pp/present.fp", "shaders_gles/pp/present");
	}
	mShader->Bind();
}

}
