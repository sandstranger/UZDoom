/*
** gl_interface.cpp
**
** OpenGL system interface
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2019 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "gl_system.h"
#include "engineerrors.h"
#include "tarray.h"
#include "basics.h"
#include "m_argv.h"
#include "version.h"
#include "v_video.h"
#include "printf.h"
#include "gl_interface.h"
#ifdef __ANDROID__
#include <SDL.h>
#endif

static TArray<FString>  m_Extensions;
RenderContext gl;
static double realglversion;
static bool bindless;

FARG(glversion, "", "", "",
	"");

//==========================================================================
//
//
//
//==========================================================================

static void CollectExtensions()
{
	const char *extension;

	int max = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &max);

	// Use modern method to collect extensions
	for (int i = 0; i < max; i++)
	{
		extension = (const char*)glGetStringi(GL_EXTENSIONS, i);
		m_Extensions.Push(FString(extension));
	}
}

//==========================================================================
//
//
//
//==========================================================================

static bool CheckExtension(const char *ext)
{
	for (unsigned int i = 0; i < m_Extensions.Size(); ++i)
	{
		if (m_Extensions[i].CompareNoCase(ext) == 0) return true;
	}

	return false;
}



//==========================================================================
//
//
//
//==========================================================================

static void InitContext()
{
	gl.flags=0;
}

//==========================================================================
//
//
//
//==========================================================================

#define FUDGE_FUNC(name, ext) 	if (_ptrc_##name == NULL) _ptrc_##name = _ptrc_##name##ext;

//==========================================================================
//
// OpenGL ES context (Android): the renderer uses the GLES 3.0 feature set,
// whatever version the driver created. Everything optional is taken from the
// extension list and the entry points actually present - no desktop version is
// substituted.
//
//==========================================================================

static void *GetESProc(const char *name)
{
#ifdef __ANDROID__
	return SDL_GL_GetProcAddress(name);
#else
	return nullptr;
#endif
}

// Multisampled renderbuffers: GL_MAX_SAMPLES, lowered to the largest count glGetInternalformativ
// reports for any of the scene formats (RGBA8 color, RGBA8/RGB10_A2 G-buffer attachments,
// packed depth/stencil) - a count above it is an error for that format.
static int MaxSceneRenderbufferSamples()
{
	static const GLenum formats[] = { GL_RGBA8, GL_RGB10_A2, GL_DEPTH24_STENCIL8 };
	GLint limit = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &limit);
	for (GLenum format : formats)
	{
		GLint count = 0, best = 0;
		glGetInternalformativ(GL_RENDERBUFFER, format, GL_NUM_SAMPLE_COUNTS, 1, &count);
		if (count > 0) glGetInternalformativ(GL_RENDERBUFFER, format, GL_SAMPLES, 1, &best);
		limit = min(limit, best);
	}
	return limit >= 2 ? limit : 0;
}

static void LoadESCapabilities()
{
	int v = 0;
	if (gl.esphysical < 300)
		I_FatalError("The OpenGL renderer needs OpenGL ES 3.0 (context: %s).\n", (const char *)glGetString(GL_VERSION));

	gl.esversion = 300;
	realglversion = gl.esphysical / 100.0;

	// Dialect of the engine's GLSL sources that is converted to GLSL ES 3.00: the uniform
	// buffer light/bone path (no shader storage), sampler bindings assigned from C++.
	gl.glslversion = 3.31f;

	// GLES 3.0 has no gl_ClipDistance; glInvalidateFramebuffer is core.
	gl.flags |= RFL_NO_CLIP_PLANES | RFL_INVALIDATE_BUFFER;
	if (CheckExtension("GL_KHR_debug")) gl.flags |= RFL_DEBUG;

	gl.floatlinear = CheckExtension("GL_OES_texture_float_linear");
	gl.bgra = CheckExtension("GL_EXT_texture_format_BGRA8888");
	gl.colorbuffer32f = CheckExtension("GL_EXT_color_buffer_float");
	gl.colorbuffer16f = gl.colorbuffer32f || CheckExtension("GL_EXT_color_buffer_half_float");

	// Multisampled render-to-texture (inline resolve on tile-based GPUs). EXT and IMG are
	// separate APIs with their own entry points and limits.
	if (CheckExtension("GL_EXT_multisampled_render_to_texture"))
	{
		gl.pFramebufferTexture2DMultisample = GetESProc("glFramebufferTexture2DMultisampleEXT");
		gl.pRenderbufferStorageMultisampleRTT = GetESProc("glRenderbufferStorageMultisampleEXT");
		if (gl.pFramebufferTexture2DMultisample && gl.pRenderbufferStorageMultisampleRTT)
		{
			gl.msrtt = 1;
			glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES_EXT */, &gl.msrttMaxSamples);
		}
	}
	if (gl.msrtt == 0 && CheckExtension("GL_IMG_multisampled_render_to_texture"))
	{
		gl.pFramebufferTexture2DMultisample = GetESProc("glFramebufferTexture2DMultisampleIMG");
		gl.pRenderbufferStorageMultisampleRTT = GetESProc("glRenderbufferStorageMultisampleIMG");
		if (gl.pFramebufferTexture2DMultisample && gl.pRenderbufferStorageMultisampleRTT)
		{
			gl.msrtt = 2;
			glGetIntegerv(0x9135 /* GL_MAX_SAMPLES_IMG */, &gl.msrttMaxSamples);
		}
	}
	if (gl.msrtt == 0 || gl.msrttMaxSamples < 2)
	{
		gl.msrtt = gl.msrttMaxSamples = 0;
		gl.pFramebufferTexture2DMultisample = gl.pRenderbufferStorageMultisampleRTT = nullptr;
	}
	gl.msaaMaxSamples = MaxSceneRenderbufferSamples();

	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &v);
	gl.maxuniforms = v;
	glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
	gl.maxuniformblock = min(65536, v);
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
	gl.uniformblockalignment = v;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_texturesize);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	bindless = false;

	Printf("OpenGL ES %d.%d context, renderer limited to GLES 3.0; float16 targets %s, float32 targets %s, float32 filtering %s, MSAA render-to-texture %s (max %d), MSAA renderbuffers max %d\n",
		gl.esphysical / 100, (gl.esphysical / 10) % 10, gl.colorbuffer16f ? "yes" : "no", gl.colorbuffer32f ? "yes" : "no", gl.floatlinear ? "yes" : "no",
		gl.msrtt == 1 ? "EXT" : gl.msrtt == 2 ? "IMG" : "no", gl.msrttMaxSamples, gl.msaaMaxSamples);
}


void gl_LoadExtensions()
{
	int v = 0;

	InitContext();
	CollectExtensions();

	const char *glversion = (const char*)glGetString(GL_VERSION);

	gl.esphysical = gl.esversion = 0;
	gl.msrtt = gl.msrttMaxSamples = gl.msaaMaxSamples = 0;
	gl.pFramebufferTexture2DMultisample = gl.pRenderbufferStorageMultisampleRTT = nullptr;
	gl.vendorstring = (char*)glGetString(GL_VENDOR);
	gl.modelstring = (char*)glGetString(GL_RENDERER);
	if (glversion && strncmp(glversion, "OpenGL ES ", 10) == 0)
	{
		int major = 0, minor = 0;
		if (sscanf(glversion + 10, "%d.%d", &major, &minor) >= 1)
			gl.esphysical = major * 100 + minor * 10;
		LoadESCapabilities();
		return;
	}
	gl.floatlinear = gl.colorbuffer16f = gl.colorbuffer32f = gl.bgra = true;

	const char *version = Args->CheckValue(FArg_glversion);
	realglversion = strtod(glversion, NULL);


	if (version == NULL)
	{
		version = glversion;
	}
	else
	{
		double v1 = strtod(version, NULL);
		if (v1 >= 3.0 && v1 < 3.3)
		{
			v1 = 3.3;	// promote '3' to 3.3 to avoid falling back to the legacy path.
			version = "3.3";
		}
		if (realglversion < v1) version = glversion;
		else Printf("Emulating OpenGL v %s\n", version);
	}

	float gl_version = (float)strtod(version, NULL) + 0.01f;

	// Don't even start if it's lower than 2.0 or no framebuffers are available (The framebuffer extension is needed for glGenerateMipmapsEXT!)
	if (gl_version < 3.3f)
	{
		I_FatalError("Unsupported OpenGL version.\nAt least OpenGL 3.3 is required to run " GAMENAME ".\n");
	}


	// add 0.01 to account for roundoff errors making the number a tad smaller than the actual version
	gl.glslversion = strtod((char*)glGetString(GL_SHADING_LANGUAGE_VERSION), NULL) + 0.01f;

	// first test for optional features
	if (CheckExtension("GL_ARB_texture_compression")) gl.flags |= RFL_TEXTURE_COMPRESSION;
	if (CheckExtension("GL_EXT_texture_compression_s3tc")) gl.flags |= RFL_TEXTURE_COMPRESSION_S3TC;

	if (CheckExtension("GL_ATI_meminfo")) gl.flags |= RFL_ATI_MEM;
	if (CheckExtension("GL_NVX_gpu_memory_info")) gl.flags |= RFL_NV_MEM;

	if (gl_version < 4.f)
	{
#ifdef _WIN32
		if (strstr(gl.vendorstring, "ATI Tech"))
		{
			gl.flags |= RFL_NO_CLIP_PLANES;	// gl_ClipDistance is horribly broken on ATI GL3 drivers for Windows.
		}
#endif
		gl.glslversion = 3.31f;	// Force GLSL down to 3.3.
	}
	else if (gl_version >= 4.5f)
	{
		// Assume that everything works without problems on GL 4.5 drivers where these things are core features.
		gl.flags |= RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;

		// Mesa implements shader storage only for fragment shaders.
		// Just disable the feature there. The light buffer may just use a uniform buffer without any adverse effects.
		glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v);
		if (v == 0)
			gl.flags &= ~RFL_SHADER_STORAGE_BUFFER;
	}



	if (gl_version >= 4.3f || CheckExtension("GL_ARB_invalidate_subdata")) gl.flags |= RFL_INVALIDATE_BUFFER;
	if (gl_version >= 4.3f || CheckExtension("GL_KHR_debug")) gl.flags |= RFL_DEBUG;

	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &v);
	gl.maxuniforms = v;
	glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
	gl.maxuniformblock = min(65536, v);
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
	gl.uniformblockalignment = v;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_texturesize);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glGetIntegerv(GL_MAX_SAMPLES, &gl.msaaMaxSamples);

	bindless = CheckExtension("GL_ARB_bindless_texture");
}

//==========================================================================
//
//
//
//==========================================================================

void gl_PrintStartupLog()
{
	int v = 0;
	// GL_CONTEXT_PROFILE_MASK and GL_MAX_VARYING_FLOATS do not exist in OpenGL ES.
	if (gl.esversion == 0) glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &v);

	Printf ("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
	Printf ("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	if (gl.esversion == 0)
		Printf ("GL_VERSION: %s (%s profile)\n", glGetString(GL_VERSION), (v & GL_CONTEXT_CORE_PROFILE_BIT)? "Core" : "Compatibility");
	else
		Printf ("GL_VERSION: %s (used as OpenGL ES 3.0)\n", glGetString(GL_VERSION));
	Printf ("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	Printf (PRINT_LOG, "GL_EXTENSIONS:");
	for (unsigned i = 0; i < m_Extensions.Size(); i++)
	{
		Printf(PRINT_LOG, " %s", m_Extensions[i].GetChars());
	}

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
	Printf("\nMax. texture size: %d\n", v);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
	Printf ("Max. texture units: %d\n", v);
	if (gl.esversion == 0)
	{
		glGetIntegerv(GL_MAX_VARYING_FLOATS, &v);
		Printf ("Max. varying: %d\n", v);
	}
	else
	{
		glGetIntegerv(0x8DFC /* GL_MAX_VARYING_VECTORS */, &v);
		Printf ("Max. varying vectors: %d\n", v);
	}

	if (gl.flags & RFL_SHADER_STORAGE_BUFFER)
	{
		glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &v);
		Printf("Max. combined shader storage blocks: %d\n", v);
		glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v);
		Printf("Max. vertex shader storage blocks: %d\n", v);
	}
	else
	{
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
		Printf("Max. uniform block size: %d\n", v);
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
		Printf("Uniform block alignment: %d\n", v);
	}
}

void setGlVersion(double glv)
{
	realglversion = glv;
}

std::pair<double, bool> gl_getInfo()
{
	// gl_ARB_bindless_texture is the closest we can get to determine Vulkan support from OpenGL.
	// This isn't foolproof because Intel doesn't support it but for NVidia and AMD support of this extension means Vulkan support.
	return std::make_pair(realglversion, bindless);
}
