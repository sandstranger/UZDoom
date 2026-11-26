/*
** gles_system.cpp
**
** GLSL shader handling
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "gles_system.h"
#include "tarray.h"
#include "v_video.h"
#include "printf.h"

CVAR(Bool, gles_use_mapped_buffer, false, 0);
CVAR(Bool, gles_force_glsl_v100, false, 0);
CVAR(Int, gles_max_lights_per_surface, 32, 0);
EXTERN_CVAR(Bool, gl_customshader);
void setGlVersion(double glv);


#if USE_GLAD_LOADER

PFNGLMAPBUFFERRANGEEXTPROC glMapBufferRange = NULL;
PFNGLUNMAPBUFFEROESPROC glUnmapBuffer = NULL;
PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer = NULL;
PFNGLFENCESYNCPROC glFenceSync = NULL;
PFNGLCLIENTWAITSYNCPROC glClientWaitSync = NULL;
PFNGLDELETESYNCPROC glDeleteSync = NULL;

#if defined _WIN32

#include <windows.h>

static HMODULE opengl32dll;
static PROC(WINAPI* getprocaddress)(LPCSTR name);

static void* LoadGLES2Proc(const char* name)
{
	HINSTANCE hGetProcIDDLL = LoadLibraryA("libGLESv2.dll");

	int error =	GetLastError();

	void* addr = GetProcAddress(hGetProcIDDLL, name);
	if (!addr)
	{
		//exit(1);
		return nullptr;
	}
	else
	{
		return addr;
	}
}

#else

#include <dlfcn.h>

static void* LoadGLES2Proc(const char* name)
{
	static void* glesLib = NULL;

	if (!glesLib)
	{
		int flags = RTLD_LOCAL | RTLD_NOW;

		glesLib = dlopen("libGLESv2_CM.so", flags);
		if (!glesLib)
		{
			glesLib = dlopen("libGLESv2.so", flags);
		}
		if (!glesLib)
		{
			glesLib = dlopen("libGLESv2.so.2", flags);
		}
	}

	void* ret = NULL;
	ret = dlsym(glesLib, name);

	return ret;
}

#endif

#endif // USE_GLAD_LOADER

static TArray<FString>  m_Extensions;

#ifdef __ANDROID__
#include <SDL.h>
#endif

// Version of the context that was actually created: 200/300/310/320 for OpenGL ES, 0 for desktop GL.
static int ContextESVersion()
{
	const char *v = (const char *)glGetString(GL_VERSION);
	int major = 0, minor = 0;
	if (v && strncmp(v, "OpenGL ES", 9) == 0 && sscanf(v + 9, "%*[^0-9]%d.%d", &major, &minor) >= 1)
		return major * 100 + minor * 10;
	return 0;
}

static void *GetProc(const char *name)
{
#ifdef __ANDROID__
	return SDL_GL_GetProcAddress(name);
#else
	return nullptr;
#endif
}

static void CollectExtensions()
{
	m_Extensions.Clear();
	if (ContextESVersion() >= 300 && glGetStringi)
	{
		// GLES 3.0: the indexed query (GL_EXTENSIONS through glGetString is the GLES 2.0 way)
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count; i++)
		{
			const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, i);
			if (ext) m_Extensions.Push(FString(ext));
		}
		if (count > 0) return;
	}
	const char* supported = (char*)glGetString(GL_EXTENSIONS);

	if (nullptr != supported)
	{
		char* extensions = new char[strlen(supported) + 1];
		strcpy(extensions, supported);

		char* extension = strtok(extensions, " ");

		while (extension)
		{
			m_Extensions.Push(FString(extension));
			extension = strtok(nullptr, " ");
		}

		delete[] extensions;
	}
}


static bool CheckExtension(const char* ext)
{
	for (unsigned int i = 0; i < m_Extensions.Size(); ++i)
	{
		if (m_Extensions[i].CompareNoCase(ext) == 0) return true;
	}

	return false;
}

namespace OpenGLESRenderer
{
	RenderContextGLES gles;

	void InitGLES()
	{

#if USE_GLAD_LOADER

		if (!gladLoadGLES2Loader(&LoadGLES2Proc))
		{
			exit(-1);
		}

		glMapBufferRange = (PFNGLMAPBUFFERRANGEEXTPROC)LoadGLES2Proc("glMapBufferRange");
		glUnmapBuffer = (PFNGLUNMAPBUFFEROESPROC)LoadGLES2Proc("glUnmapBuffer");
		glVertexAttribIPointer = (PFNGLVERTEXATTRIBIPOINTERPROC)LoadGLES2Proc("glVertexAttribIPointer");

		glFenceSync = (PFNGLFENCESYNCPROC)LoadGLES2Proc("glFenceSync");
		glClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)LoadGLES2Proc("glClientWaitSync");
		glDeleteSync = (PFNGLDELETESYNCPROC)LoadGLES2Proc("glDeleteSync");
#else
		static bool first = true;

		if (first)
		{
			if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
			{
				//I_FatalError("Failed to load OpenGL functions.");
			}
		}
		// Vertex array objects are GLES 3.0 core; a GLES 2.0 context only has them through
		// OES_vertex_array_object, which this loader does not resolve. Default VAO there.
		const int esContext = ContextESVersion();
		if ((esContext == 0 || esContext >= 300) && glGenVertexArrays && glBindVertexArray)
		{
			GLuint vao;
			glGenVertexArrays(1, &vao);
			glBindVertexArray(vao);
		}
#endif
		CollectExtensions();

		Printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
		Printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
		Printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
		Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
		Printf(PRINT_LOG, "GL_EXTENSIONS:\n");
		for (unsigned i = 0; i < m_Extensions.Size(); i++)
		{
			Printf(PRINT_LOG, " %s\n", m_Extensions[i].GetChars());
		}
		const char* glVersionStr = (const char*)glGetString(GL_VERSION);
		double glVersion = strtod(glVersionStr, NULL);
		// The version of the created context, not of the requested one.
		gles.esVersion = ContextESVersion();
		if (gles.esVersion > 0) glVersion = gles.esVersion / 100.0;

		Printf("GL Version parsed = %f\n", glVersion);

		gles.flags = RFL_NO_CLIP_PLANES;
		gles.essl300 = false;

		gles.useMappedBuffers = gles_use_mapped_buffer;
		gles.forceGLSLv100 = gles_force_glsl_v100;
		gles.maxlights = gles_max_lights_per_surface;
		gles.numlightvectors = (gles.maxlights * LIGHT_VEC4_NUM);

		gles.modelstring = (char*)glGetString(GL_RENDERER);
		gles.vendorstring = (char*)glGetString(GL_VENDOR);
		{
			const char *v = (const char*)glGetString(GL_VERSION);
			gles.angle = (gles.modelstring && strstr(gles.modelstring, "ANGLE")) || (v && strstr(v, "ANGLE"));
		}


		GLint maxTextureSize[1];
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, maxTextureSize);
		gles.max_texturesize = maxTextureSize[0];

		Printf("GL_MAX_TEXTURE_SIZE: %d\n", gles.max_texturesize);


		// Check if running on a GLES device, version string will start with 'OpenGL ES'
#ifdef ANDROID //karin: custom GL version
		if(USING_GLES_AUTO) {
#endif
		if (!strncmp(glVersionStr, "OpenGL ES", strlen("OpenGL ES")))
		{
			gles.glesMode = GLES_MODE_GLES;
		}
		else // Else runnning on Desktop, check OpenGL version is 3 or above
		{
			if (glVersion > 3.29)
				gles.glesMode = GLES_MODE_OGL3; // 3.3 or above
			else
				gles.glesMode = GLES_MODE_OGL2; // Below 3.3
		}
#ifdef ANDROID //karin: custom GL version
		} else {
			if (USING_GLES_2)
				gles.glesMode = GLES_MODE_OGL2; // GLES 2.0 context, GLSL ES 1.00
			else if (gles.esVersion >= 300)
				gles.glesMode = GLES_MODE_GLES3; // GLES 3.0 context, GLSL ES 3.00
			else
			{
				Printf("OpenGL ES 3.0 requested but the context is %s: using the OpenGL ES 2.0 path\n", glVersionStr);
				gles.glesMode = GLES_MODE_OGL2;
			}
		}
		if (USING_GLES_AUTO && gles.esVersion >= 300)
			gles.glesMode = GLES_MODE_GLES3;
#endif

		gles.nv_vram_ext = CheckExtension("GL_NVX_gpu_memory_info");
		gles.ati_vram_ext = CheckExtension("GL_ATI_meminfo");

		// Compressed texture formats. ETC2 only on the GLES 3.0 path (and desktop GL 3.3+ with ES3 compatibility):
		// the GLES 2.0 path uses GLES 2.0 features only, the BC images it cannot take as they are get decoded.
		gles.s3tcTextures = CheckExtension("GL_EXT_texture_compression_s3tc");
		gles.s3tcSRGBTextures = gles.esVersion > 0 ? CheckExtension("GL_EXT_texture_compression_s3tc_srgb") : gles.s3tcTextures;
		gles.bptcTextures = gles.esVersion > 0 ? CheckExtension("GL_EXT_texture_compression_bptc") :
			(glVersion >= 4.2 || CheckExtension("GL_ARB_texture_compression_bptc"));
		gles.texMaxLevel = gles.glesMode == GLES_MODE_GLES3 || gles.glesMode == GLES_MODE_OGL3;
		gles.etc2Textures = gles.glesMode == GLES_MODE_GLES3 ||
			(gles.glesMode == GLES_MODE_OGL3 && (glVersion >= 4.3 || CheckExtension("GL_ARB_ES3_compatibility")));

		if (gles.glesMode == GLES_MODE_GLES)
		{
			Printf("GLES choosing mode: GLES_MODE_GLES\n");

			gles.shaderVersionString = "100";
			gles.depthStencilAvailable = CheckExtension("GL_OES_packed_depth_stencil");
			gles.npotAvailable = CheckExtension("GL_OES_texture_npot");
			gles.depthClampAvailable = CheckExtension("GL_EXT_depth_clamp");
			gles.anistropicFilterAvailable = CheckExtension("GL_EXT_texture_filter_anisotropic");
		}
		else if (gles.glesMode == GLES_MODE_OGL2)
		{
			Printf("GLES choosing mode: GLES_MODE_OGL2\n");

			gles.shaderVersionString = "100";
#ifdef ANDROID
			// OpenGL ES 2.0 (also on a newer context: the GLES 2.0 path uses GLES 2.0 features only)
			gles.depthStencilAvailable = CheckExtension("GL_OES_packed_depth_stencil");
			gles.npotAvailable = CheckExtension("GL_OES_texture_npot");
			gles.depthClampAvailable = CheckExtension("GL_EXT_depth_clamp");
			gles.anistropicFilterAvailable = CheckExtension("GL_EXT_texture_filter_anisotropic");
			// glMapBufferRange/glUnmapBuffer are GLES 3.0 core; their GLES 2.0 extension forms are not loaded.
			gles.useMappedBuffers = false;
#else
			gles.depthStencilAvailable = true;
			gles.npotAvailable = true;
			gles.useMappedBuffers = true;
			gles.depthClampAvailable = true;
			gles.anistropicFilterAvailable = true;
#endif
		}
		else if (gles.glesMode == GLES_MODE_OGL3)
		{
			Printf("GLES choosing mode: GLES_MODE_OGL3\n");

			gles.shaderVersionString = "330";
			gles.depthStencilAvailable = true;
			gles.npotAvailable = true;
			gles.useMappedBuffers = true;
			gles.depthClampAvailable = true;
			gles.anistropicFilterAvailable = true;
		}
		else if (gles.glesMode == GLES_MODE_GLES3)
		{
			Printf("GLES choosing mode: GLES_MODE_GLES3\n");

			// OpenGL ES 3.0: the GLSL ES 1.00 sources are converted to GLSL ES 3.00. Packed
			// depth/stencil and NPOT textures with mipmaps and repeat are core, independent of
			// the OES extension strings. Nothing beyond GLES 3.0 is used on 3.1/3.2 contexts.
			gles.shaderVersionString = "300 es";
			gles.essl300 = true;
			gles.depthStencilAvailable = true;
			gles.npotAvailable = true;
			gles.depthClampAvailable = CheckExtension("GL_EXT_depth_clamp");
			gles.anistropicFilterAvailable = CheckExtension("GL_EXT_texture_filter_anisotropic");
		}

		// Capability table. A GLES 3.0 feature is used when the mode is the GLES 3.0 one (or
		// desktop GL 3.3) and its entry points were resolved.
		const bool es3 = gles.glesMode == GLES_MODE_GLES3;
		const bool core3 = es3 || gles.glesMode == GLES_MODE_OGL3;

		// Mods' material shaders (GLDEFS) are written for the GL renderer's GLSL: GLSL ES 3.00 can
		// take them (with NG-GL4ES converting what the driver refuses), GLSL ES 1.00 cannot.
		if (!es3) gl_customshader = false;
		gles.bgraTextures = CheckExtension("GL_EXT_texture_format_BGRA8888");
		gles.textureSwizzle = core3;
		gles.syncObjects = core3 && glFenceSync && glClientWaitSync && glDeleteSync;
		gles.bufferStorage = false;
		gles.pBufferStorage = nullptr;
		if (es3 && gles.syncObjects && glMapBufferRange && glFlushMappedBufferRange && CheckExtension("GL_EXT_buffer_storage"))
		{
			gles.pBufferStorage = GetProc("glBufferStorageEXT");
			gles.bufferStorage = gles.pBufferStorage != nullptr;
		}
		gles.integerAttribs = core3 && glVertexAttribIPointer;

		// Selaco's lightmaps: on the last units (the material layers have 0-11: 'tex' and texture2..texture12)
		{
			GLint units = 8;
			glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
			if (units > 16) units = 16;	// (IHardwareTexture::MAX_TEXTURES: the units the binding tracking knows)
			gles.lightmapArrays = core3 && glTexImage3D && glFramebufferTextureLayer;
			gles.lightmapUnitCount = gles.lightmapArrays ? 1 : 4;
			gles.lightmapUnit = units - gles.lightmapUnitCount;
			if (gles.lightmapUnit < 1) gles.lightmapUnit = 1;
		}
		gles.invalidateFramebuffer = es3 && glInvalidateFramebuffer;
		gles.pDiscardFramebuffer = nullptr;
		if (!es3 && gles.esVersion > 0)
		{
			// GLES 2.0 path: EXT_discard_framebuffer. A GLES 3.x context that does not list it has
			// the same operation as core glInvalidateFramebuffer (only on a real 3.x context: a
			// GLES 2.0 driver may hand out a pointer for any name).
			if (CheckExtension("GL_EXT_discard_framebuffer"))
				gles.pDiscardFramebuffer = GetProc("glDiscardFramebufferEXT");
			else if (gles.esVersion >= 300 && glInvalidateFramebuffer)
				gles.invalidateFramebuffer = true;
		}

		gles.msrtt = gles.msrttMaxSamples = 0;
		gles.pFramebufferTexture2DMultisample = gles.pRenderbufferStorageMultisampleRTT = nullptr;
		if (gles.esVersion > 0)
		{
			// EXT and IMG multisampled render-to-texture are different APIs with their own entry points.
			if (CheckExtension("GL_EXT_multisampled_render_to_texture"))
			{
				gles.pFramebufferTexture2DMultisample = GetProc("glFramebufferTexture2DMultisampleEXT");
				gles.pRenderbufferStorageMultisampleRTT = GetProc("glRenderbufferStorageMultisampleEXT");
				if (gles.pFramebufferTexture2DMultisample && gles.pRenderbufferStorageMultisampleRTT)
				{
					gles.msrtt = 1;
					glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES_EXT */, &gles.msrttMaxSamples);
				}
			}
			if (gles.msrtt == 0 && CheckExtension("GL_IMG_multisampled_render_to_texture"))
			{
				gles.pFramebufferTexture2DMultisample = GetProc("glFramebufferTexture2DMultisampleIMG");
				gles.pRenderbufferStorageMultisampleRTT = GetProc("glRenderbufferStorageMultisampleIMG");
				if (gles.pFramebufferTexture2DMultisample && gles.pRenderbufferStorageMultisampleRTT)
				{
					gles.msrtt = 2;
					glGetIntegerv(0x9135 /* GL_MAX_SAMPLES_IMG */, &gles.msrttMaxSamples);
				}
			}
			if (gles.msrttMaxSamples < 2)
			{
				gles.msrtt = gles.msrttMaxSamples = 0;
				gles.pFramebufferTexture2DMultisample = gles.pRenderbufferStorageMultisampleRTT = nullptr;
			}
		}
		gles.maxSamples = 0;
		gles.pRenderbufferStorageMultisample = gles.pBlitFramebuffer = nullptr;
		if (core3 && glRenderbufferStorageMultisample && glBlitFramebuffer)
		{
			gles.pRenderbufferStorageMultisample = (void*)glRenderbufferStorageMultisample;
			gles.pBlitFramebuffer = (void*)glBlitFramebuffer;
		}
		else if (gles.esVersion > 0 && gles.depthStencilAvailable && (CheckExtension("GL_OES_rgb8_rgba8") || CheckExtension("GL_ARM_rgba8")))
		{
			// GLES 2.0: multisampled renderbuffers and their resolve blit from the extensions that have them -
			// ANGLE's GLES 2.0 contexts and Tegra (NV). Same enums as GLES 3.0 (GL_MAX_SAMPLES, the read/draw
			// frame buffer targets). The scene is RGBA8 with packed depth/stencil: both need their extensions.
			if (CheckExtension("GL_ANGLE_framebuffer_multisample") && CheckExtension("GL_ANGLE_framebuffer_blit"))
			{
				gles.pRenderbufferStorageMultisample = GetProc("glRenderbufferStorageMultisampleANGLE");
				gles.pBlitFramebuffer = GetProc("glBlitFramebufferANGLE");
			}
			else if (CheckExtension("GL_NV_framebuffer_multisample") && CheckExtension("GL_NV_framebuffer_blit"))
			{
				gles.pRenderbufferStorageMultisample = GetProc("glRenderbufferStorageMultisampleNV");
				gles.pBlitFramebuffer = GetProc("glBlitFramebufferNV");
			}
			if (!gles.pRenderbufferStorageMultisample || !gles.pBlitFramebuffer)
				gles.pRenderbufferStorageMultisample = gles.pBlitFramebuffer = nullptr;
		}
		gles.msaaRenderbuffers = gles.pRenderbufferStorageMultisample && gles.pBlitFramebuffer;
		if (gles.msaaRenderbuffers)
		{
			glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES (= _ANGLE, _NV) */, &gles.maxSamples);
			if (es3 && glGetInternalformativ)
			{
				// lowered to the largest count each scene format reports (a higher one is an
				// error for that format)
				static const GLenum formats[] = { GL_RGBA8, GL_DEPTH24_STENCIL8 };
				for (GLenum format : formats)
				{
					GLint count = 0, best = 0;
					glGetInternalformativ(GL_RENDERBUFFER, format, GL_NUM_SAMPLE_COUNTS, 1, &count);
					if (count > 0) glGetInternalformativ(GL_RENDERBUFFER, format, GL_SAMPLES, 1, &best);
					gles.maxSamples = std::min<int>(gles.maxSamples, best);
				}
			}
		}
		if (gles.maxSamples < 2) gles.msaaRenderbuffers = false, gles.maxSamples = 0;

		// Post-process render targets (GLES 3.0 mode): the float formats that can be rendered to
		// (core in GLES 3.2) and filtered.
		gles.colorbuffer32f = es3 && (gles.esVersion >= 320 || CheckExtension("GL_EXT_color_buffer_float"));
		gles.colorbuffer16f = es3 && (gles.colorbuffer32f || CheckExtension("GL_EXT_color_buffer_half_float"));
		gles.floatlinear = CheckExtension("GL_OES_texture_float_linear");

		Printf("GLES capabilities%s: BGRA textures %s, sync %s, invalidate %s, discard %s, MSAA render-to-texture %s (max %d), MSAA renderbuffers %s (max %d), float render targets 16f %s 32f %s, float linear %s\n",
			gles.angle ? " (ANGLE)" : "", gles.bgraTextures ? "yes" : "no", gles.syncObjects ? "yes" : "no", gles.invalidateFramebuffer ? "yes" : "no",
			gles.pDiscardFramebuffer ? "yes" : "no", gles.msrtt == 1 ? "EXT" : gles.msrtt == 2 ? "IMG" : "no", gles.msrttMaxSamples,
			gles.msaaRenderbuffers ? "yes" : "no", gles.maxSamples, gles.colorbuffer16f ? "yes" : "no", gles.colorbuffer32f ? "yes" : "no",
			gles.floatlinear ? "yes" : "no");

		setGlVersion(glVersion);
	}
}
