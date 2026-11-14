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

#ifdef __ANDROID__ //karin: force GL version
extern float GLimp_GetGLVersion(void);
#endif

static void CollectExtensions()
{
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
		if (GLimp_GetGLVersion() >= 3.0f && glGenVertexArrays && glBindVertexArray)
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
#ifdef __ANDROID__ //karin: force GL version
		glVersion = GLimp_GetGLVersion();
#endif

		Printf("GL Version parsed = %f\n", glVersion);

		gles.flags = RFL_NO_CLIP_PLANES;
		gles.essl300 = false;

		gles.useMappedBuffers = gles_use_mapped_buffer;
		gles.forceGLSLv100 = gles_force_glsl_v100;
		gles.maxlights = gles_max_lights_per_surface;
		gles.numlightvectors = (gles.maxlights * LIGHT_VEC4_NUM);

		gles.modelstring = (char*)glGetString(GL_RENDERER);
		gles.vendorstring = (char*)glGetString(GL_VENDOR);


		gl_customshader = false; // Disable user shaders for GLES renderer

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
			if(USING_GLES_2)
				gles.glesMode = GLES_MODE_OGL2; // GLES 2.0 context, GLSL ES 1.00
			else
				gles.glesMode = GLES_MODE_OGL3; // GLES 3.0 context, GLSL ES 3.00
		}
#endif

		gles.nv_vram_ext = CheckExtension("GL_NVX_gpu_memory_info");
		gles.ati_vram_ext = CheckExtension("GL_ATI_meminfo");

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
#ifdef ANDROID //karin: check extensions on OpenGLES
			gles.depthStencilAvailable = CheckExtension("GL_OES_packed_depth_stencil");
			gles.npotAvailable = CheckExtension("GL_OES_texture_npot");
			gles.depthClampAvailable = CheckExtension("GL_EXT_depth_clamp");
			gles.anistropicFilterAvailable = CheckExtension("GL_EXT_texture_filter_anisotropic");
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

#ifdef ANDROID
			// GLES 3.0 context: the GLSL ES 1.00 sources are converted to GLSL ES 3.00 (needed by
			// the instanced shader flavours). Packed depth/stencil and NPOT textures with mipmaps
			// and repeat are core in GLES 3.0, independent of the OES extension strings.
			gles.shaderVersionString = "300 es";
			gles.essl300 = true;
			gles.depthStencilAvailable = true;
			gles.npotAvailable = true;
			gles.depthClampAvailable = CheckExtension("GL_EXT_depth_clamp");
			gles.anistropicFilterAvailable = CheckExtension("GL_EXT_texture_filter_anisotropic");
#else
			gles.shaderVersionString = "330";
			gles.depthStencilAvailable = true;
			gles.npotAvailable = true;
			gles.useMappedBuffers = true;
			gles.depthClampAvailable = true;
			gles.anistropicFilterAvailable = true;
#endif
		}

		setGlVersion(glVersion);
	}
}
