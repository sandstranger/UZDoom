/*
** gles_renderbuffers.cpp
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
#include "printf.h"
#include "hw_cvars.h"
#include "gles_renderer.h"
#include "gles_renderbuffers.h"
#include "gles_postprocessstate.h"
#include "gles_shaderprogram.h"
#include "gles_buffers.h"

#include <random>

EXTERN_CVAR(Int, gl_debug_level)

#include "hw_batching.h"
#include <algorithm>
EXTERN_CVAR(Int, gl_multisample)

namespace OpenGLESRenderer
{

	//==========================================================================
	//
	// Initialize render buffers and textures used in rendering passes
	//
	//==========================================================================

	FGLRenderBuffers::FGLRenderBuffers()
	{

	}

	//==========================================================================
	//
	// Free render buffer resources
	//
	//==========================================================================

	FGLRenderBuffers::~FGLRenderBuffers()
	{
		ClearScene();

		DeleteTexture(mDitherTexture);
	}

	void FGLRenderBuffers::ClearScene()
	{
		DeleteFrameBuffer(mSceneFB);
		DeleteFrameBuffer(mResolveFB);
		DeleteRenderBuffer(mSceneDepthStencilBuf);
		DeleteRenderBuffer(mSceneStencilBuf);
		DeleteRenderBuffer(mSceneColorMSBuf);
		mMSAAMode = MSAA_None;
		mResolvePending = false;
	}

	void FGLRenderBuffers::DeleteTexture(PPGLTexture& tex)
	{
		if (tex.handle != 0)
		{
			glDeleteTextures(1, &tex.handle);
			rtStatsGLES.deleted++;
			int64_t now = int64_t(rtStatsGLES.bytes) - int64_t(size_t(tex.Width) * size_t(tex.Height) * 4);
			rtStatsGLES.bytes = now > 0 ? size_t(now) : 0;
		}
		tex.handle = 0;
	}

	void FGLRenderBuffers::DeleteRenderBuffer(PPGLRenderBuffer& buf)
	{
		if (buf.handle != 0)
		{
			glDeleteRenderbuffers(1, &buf.handle);
			rtStatsGLES.deleted++;
		}
		buf.handle = 0;
	}

	void FGLRenderBuffers::DeleteFrameBuffer(PPGLFrameBuffer& fb)
	{
		if (fb.handle != 0)
			glDeleteFramebuffers(1, &fb.handle);
		fb.handle = 0;
	}

	//==========================================================================
	//
	// Makes sure all render buffers have sizes suitable for rending at the
	// specified resolution
	//
	//==========================================================================

	void FGLRenderBuffers::Setup(int width, int height, int sceneWidth, int sceneHeight)
	{
		if (width <= 0 || height <= 0)
			I_FatalError("Requested invalid render buffer sizes: screen = %dx%d", width, height);

		GLint activeTex;
		GLint textureBinding;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);

		// MSAA: render-to-texture (EXT/IMG, GLES 2.0 and 3.0), else multisampled renderbuffers
		// with a blit resolve (GLES 3.0), else none.
		int samples = gl_multisample >= 2 ? (int)gl_multisample : 0;
		if (samples)
		{
			if (gles.msrtt) samples = std::min(samples, gles.msrttMaxSamples);
			else if (gles.msaaRenderbuffers) samples = std::min(samples, gles.maxSamples);
			else samples = 0;
		}

		const bool resized = width != mWidth || height != mHeight;
		if (resized)
		{
			// the frame buffer references the old texture: release both before creating the new set
			ClearScene();
			CreatePipeline(width, height);
		}

		if (resized || samples != mSamples)
			CreateScene(width, height, samples);
		mSamples = samples;

		mWidth = width;
		mHeight = height;
		mSceneWidth = sceneWidth;
		mSceneHeight = sceneHeight;

		glBindTexture(GL_TEXTURE_2D, textureBinding);
		glActiveTexture(activeTex);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		if (FailedCreate)
		{
			ClearScene();
			mWidth = 0;
			mHeight = 0;
			mSceneWidth = 0;
			mSceneHeight = 0;
			I_FatalError("Unable to create render buffers.");
		}
	}

	//==========================================================================
	//
	// Creates the scene buffers
	//
	//==========================================================================

	void FGLRenderBuffers::CreateScene(int width, int height, int samples)
	{
		ClearScene();
		if (samples > 1 && gles.msrtt && CreateSceneRTT(width, height, samples)) return;
		if (samples > 1 && gles.msaaRenderbuffers && CreateSceneMultisample(width, height, samples)) return;

		if(gles.depthStencilAvailable)
			mSceneDepthStencilBuf = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH24_STENCIL8, width, height);
		else
		{
			mSceneDepthStencilBuf = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH_COMPONENT16, width, height);
			mSceneStencilBuf = CreateRenderBuffer("SceneStencil", GL_STENCIL_INDEX8, width, height);
		}
		mSceneFB= CreateFrameBuffer("SceneFB", mSceneTex, mSceneDepthStencilBuf, mSceneStencilBuf);
	}

	//==========================================================================
	//
	// Creates the buffers needed for post processing steps
	//
	//==========================================================================

	void FGLRenderBuffers::CreatePipeline(int width, int height)
	{
		DeleteTexture(mSceneTex);	// was leaked on every resolution change
		mSceneTex = Create2DTexture("PipelineTexture", GL_RGBA, width, height);
	}

	//==========================================================================
	//
	// Multisampled render-to-texture: samples stay in tile memory and are resolved
	// into mSceneTex when tiles are written out - no multisampled buffer in memory.
	//
	//==========================================================================

	bool FGLRenderBuffers::CreateSceneRTT(int width, int height, int samples)
	{
		typedef void (APIENTRY *FT2DMS)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
		if (gles.depthStencilAvailable)
			mSceneDepthStencilBuf = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH24_STENCIL8, width, height, samples, true);
		else
		{
			mSceneDepthStencilBuf = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH_COMPONENT16, width, height, samples, true);
			mSceneStencilBuf = CreateRenderBuffer("SceneStencil", GL_STENCIL_INDEX8, width, height, samples, true);
		}
		glGenFramebuffers(1, &mSceneFB.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB.handle);
		((FT2DMS)gles.pFramebufferTexture2DMultisample)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mSceneTex.handle, 0, samples);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mSceneDepthStencilBuf.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gles.depthStencilAvailable ? mSceneDepthStencilBuf.handle : mSceneStencilBuf.handle);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			Printf("Multisampled render-to-texture frame buffer incomplete, trying other MSAA paths\n");
			ClearScene();
			return false;
		}
		ClearFrameBuffer(true, true);
		mMSAAMode = MSAA_RenderToTexture;
		return true;
	}

	//==========================================================================
	//
	// GLES 3.0: multisampled renderbuffers, resolved into mSceneTex by a blit
	// (ResolveScene) before the scene texture is read.
	//
	//==========================================================================

	bool FGLRenderBuffers::CreateSceneMultisample(int width, int height, int samples)
	{
		mSceneColorMSBuf = CreateRenderBuffer("SceneMultisample", GL_RGBA8, width, height, samples, false);
		mSceneDepthStencilBuf = CreateRenderBuffer("SceneDepthStencil", GL_DEPTH24_STENCIL8, width, height, samples, false);
		glGenFramebuffers(1, &mSceneFB.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, mSceneColorMSBuf.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mSceneDepthStencilBuf.handle);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			Printf("Multisampled frame buffer incomplete, rendering without MSAA\n");
			ClearScene();
			return false;
		}
		ClearFrameBuffer(true, true);
		mResolveFB = CreateFrameBuffer("SceneResolveFB", mSceneTex);
		mMSAAMode = MSAA_Renderbuffers;
		return true;
	}

	void FGLRenderBuffers::ResolveScene()
	{
		if (mMSAAMode != MSAA_Renderbuffers || !mResolvePending) return;
		GLint fb = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mSceneFB.handle);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mResolveFB.handle);
		glBlitFramebuffer(0, 0, mWidth, mHeight, 0, 0, mWidth, mHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
		mResolvePending = false;
		rtStatsGLES.resolves++;
	}

	void FGLRenderBuffers::BindReadableFB()
	{
		ResolveScene();
		rtStatsGLES.fboBinds++;
		glBindFramebuffer(GL_FRAMEBUFFER, mMSAAMode == MSAA_Renderbuffers ? mResolveFB.handle : mSceneFB.handle);
	}

	void FGLRenderBuffers::Discard(int count, const GLenum *attachments)
	{
		typedef void (APIENTRY *DISCARDFB)(GLenum, GLsizei, const GLenum *);
		if (gles.invalidateFramebuffer)
			glInvalidateFramebuffer(GL_FRAMEBUFFER, count, attachments);
		else if (gles.pDiscardFramebuffer)
			((DISCARDFB)gles.pDiscardFramebuffer)(GL_FRAMEBUFFER, count, attachments);
	}

	void FGLRenderBuffers::InvalidateSceneDepth()
	{
		if (!mSceneFB || (!gles.invalidateFramebuffer && !gles.pDiscardFramebuffer)) return;
		GLint fb = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB.handle);
		static const GLenum attachments[] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
		Discard(2, attachments);
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
	}



	//==========================================================================
	//
	// Creates a 2D texture defaulting to linear filtering and clamp to edge
	//
	//==========================================================================

	PPGLTexture FGLRenderBuffers::Create2DTexture(const char* name, GLuint format, int width, int height, const void* data)
	{
		PPGLTexture tex;
		tex.Width = width;
		tex.Height = height;
		rtStatsGLES.created++;
		rtStatsGLES.bytes += size_t(width) * size_t(height) * 4;
		if (rtStatsGLES.bytes > rtStatsGLES.peakBytes) rtStatsGLES.peakBytes = rtStatsGLES.bytes;
		glGenTextures(1, &tex.handle);
		glBindTexture(GL_TEXTURE_2D, tex.handle);

		GLenum dataformat = 0, datatype = 0;
		/*
		switch (format)
		{
		case GL_RGBA:				dataformat = GL_RGBA; datatype = GL_UNSIGNED_BYTE; break;

		case GL_RGBA16:				dataformat = GL_RGBA; datatype = GL_UNSIGNED_SHORT; break;
		case GL_RGBA16F:			dataformat = GL_RGBA; datatype = GL_FLOAT; break;
		case GL_RGBA32F:			dataformat = GL_RGBA; datatype = GL_FLOAT; break;
		case GL_RGBA16_SNORM:		dataformat = GL_RGBA; datatype = GL_SHORT; break;
		case GL_R32F:				dataformat = GL_RED; datatype = GL_FLOAT; break;
		case GL_R16F:				dataformat = GL_RED; datatype = GL_FLOAT; break;
		case GL_RG32F:				dataformat = GL_RG; datatype = GL_FLOAT; break;
		case GL_RG16F:				dataformat = GL_RG; datatype = GL_FLOAT; break;
		case GL_RGB10_A2:			dataformat = GL_RGBA; datatype = GL_UNSIGNED_INT_10_10_10_2; break;
		case GL_DEPTH_COMPONENT24:	dataformat = GL_DEPTH_COMPONENT; datatype = GL_FLOAT; break;

		case GL_STENCIL_INDEX8:		dataformat = GL_STENCIL_INDEX8_OES; datatype = GL_INT; break;
		case GL_DEPTH24_STENCIL8_OES:	dataformat = GL_DEPTH_STENCIL_OES; datatype = GL_UNSIGNED_INT_24_8; break;
		default: I_FatalError("Unknown format passed to FGLRenderBuffers.Create2DTexture");
		}
	*/
		format = GL_RGBA;
		dataformat = GL_RGBA;
		datatype = GL_UNSIGNED_BYTE;

		glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, dataformat, datatype, data);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return tex;
	}

	//==========================================================================
	//
	// Creates a render buffer
	//
	//==========================================================================

	PPGLRenderBuffer FGLRenderBuffers::CreateRenderBuffer(const char* name, GLuint format, int width, int height)
	{
		PPGLRenderBuffer buf;
		glGenRenderbuffers(1, &buf.handle);
		glBindRenderbuffer(GL_RENDERBUFFER, buf.handle);

		glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
		rtStatsGLES.created++;
		return buf;
	}

	PPGLRenderBuffer FGLRenderBuffers::CreateRenderBuffer(const char* name, GLuint format, int width, int height, int samples, bool renderToTexture)
	{
		typedef void (APIENTRY *RBSMS)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
		PPGLRenderBuffer buf;
		glGenRenderbuffers(1, &buf.handle);
		glBindRenderbuffer(GL_RENDERBUFFER, buf.handle);
		if (renderToTexture)
			((RBSMS)gles.pRenderbufferStorageMultisampleRTT)(GL_RENDERBUFFER, samples, format, width, height);	// the extension's own entry point
		else
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, width, height);
		rtStatsGLES.created++;
		return buf;
	}

	//==========================================================================
	//
	// Creates a frame buffer
	//
	//==========================================================================

	PPGLFrameBuffer FGLRenderBuffers::CreateFrameBuffer(const char* name, PPGLTexture colorbuffer)
	{
		PPGLFrameBuffer fb;
		glGenFramebuffers(1, &fb.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer.handle, 0);
		if (CheckFrameBufferCompleteness())
			ClearFrameBuffer(false, false);
		return fb;
	}

	PPGLFrameBuffer FGLRenderBuffers::CreateFrameBuffer(const char* name, PPGLTexture colorbuffer, PPGLRenderBuffer depthstencil, PPGLRenderBuffer stencil)
	{
		PPGLFrameBuffer fb;
		glGenFramebuffers(1, &fb.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer.handle, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthstencil.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gles.depthStencilAvailable ? depthstencil.handle : stencil.handle);
		if (CheckFrameBufferCompleteness())
			ClearFrameBuffer(true, true);
		return fb;
	}

	PPGLFrameBuffer FGLRenderBuffers::CreateFrameBuffer(const char* name, PPGLRenderBuffer colorbuffer, PPGLRenderBuffer depthstencil, PPGLRenderBuffer stencil)
	{
		PPGLFrameBuffer fb;
		glGenFramebuffers(1, &fb.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);

		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorbuffer.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthstencil.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gles.depthStencilAvailable ? depthstencil.handle : stencil.handle);
		if (CheckFrameBufferCompleteness())
			ClearFrameBuffer(true, true);
		return fb;
	}

	PPGLFrameBuffer FGLRenderBuffers::CreateFrameBuffer(const char* name, PPGLTexture colorbuffer0, PPGLTexture colorbuffer1, PPGLTexture colorbuffer2, PPGLTexture depthstencil, PPGLRenderBuffer stencil)
	{
		PPGLFrameBuffer fb;
		glGenFramebuffers(1, &fb.handle);
		glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer0.handle, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthstencil.handle);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gles.depthStencilAvailable ? depthstencil.handle : stencil.handle);

		if (CheckFrameBufferCompleteness())
			ClearFrameBuffer(true, true);
		return fb;
	}

	//==========================================================================
	//
	// Verifies that the frame buffer setup is valid
	//
	//==========================================================================

	bool FGLRenderBuffers::CheckFrameBufferCompleteness()
	{
		GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (result == GL_FRAMEBUFFER_COMPLETE)
			return true;

		FString error = "glCheckFramebufferStatus failed: ";
		switch (result)
		{
		default: error.AppendFormat("error code %d", (int)result); break;
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_UNSUPPORTED: error << "GL_FRAMEBUFFER_UNSUPPORTED"; break;
		}
		Printf("%s\n", error.GetChars());


		return false;
	}

	//==========================================================================
	//
	// Clear frame buffer to make sure it never contains uninitialized data
	//
	//==========================================================================

	void FGLRenderBuffers::ClearFrameBuffer(bool stencil, bool depth)
	{
		GLboolean scissorEnabled;
		GLint stencilValue;
		GLfloat depthValue;
		glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
		glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencilValue);
		glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depthValue);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.0, 0.0, 0.0, 0.0);
		glClearDepthf(0.0f);
		glClearStencil(0);
		GLenum flags = GL_COLOR_BUFFER_BIT;
		if (stencil)
			flags |= GL_STENCIL_BUFFER_BIT;
		if (depth)
			flags |= GL_DEPTH_BUFFER_BIT;
		glClear(flags);
		glClearStencil(stencilValue);
		glClearDepthf(depthValue);
		if (scissorEnabled)
			glEnable(GL_SCISSOR_TEST);
	}


	void FGLRenderBuffers::BindDitherTexture(int texunit)
	{
		if (!mDitherTexture)
		{
			static const float data[64] =
			{
				 .0078125, .2578125, .1328125, .3828125, .0234375, .2734375, .1484375, .3984375,
				 .7578125, .5078125, .8828125, .6328125, .7734375, .5234375, .8984375, .6484375,
				 .0703125, .3203125, .1953125, .4453125, .0859375, .3359375, .2109375, .4609375,
				 .8203125, .5703125, .9453125, .6953125, .8359375, .5859375, .9609375, .7109375,
				 .0390625, .2890625, .1640625, .4140625, .0546875, .3046875, .1796875, .4296875,
				 .7890625, .5390625, .9140625, .6640625, .8046875, .5546875, .9296875, .6796875,
				 .1015625, .3515625, .2265625, .4765625, .1171875, .3671875, .2421875, .4921875,
				 .8515625, .6015625, .9765625, .7265625, .8671875, .6171875, .9921875, .7421875,
			};

			// The texture is RGBA8: the float matrix has to be converted (it used to be uploaded as
			// raw bytes, which gave a garbage dither pattern).
			uint8_t bytes[64 * 4];
			for (int i = 0; i < 64; i++)
				bytes[i * 4 + 0] = bytes[i * 4 + 1] = bytes[i * 4 + 2] = bytes[i * 4 + 3] = (uint8_t)(data[i] * 255.f + 0.5f);
			glActiveTexture(GL_TEXTURE0 + texunit);
			mDitherTexture = Create2DTexture("DitherTexture", GL_RGBA, 8, 8, bytes);
		}
		mDitherTexture.Bind(1, GL_NEAREST, GL_REPEAT);
	}


	//==========================================================================
	//
	// Makes the scene frame buffer active (multisample, depth, stecil, etc.)
	//
	//==========================================================================

	void FGLRenderBuffers::BindSceneFB(bool sceneData)
	{
		rtStatsGLES.fboBinds++;
		mResolvePending = true;
		glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB.handle);
	}

	//==========================================================================
	//
	// Binds the current scene/effect/hud texture to the specified texture unit
	//
	//==========================================================================

	void FGLRenderBuffers::BindCurrentTexture(int index, int filter, int wrap)
	{
		ResolveScene();
		mSceneTex.Bind(index, filter, wrap);
	}

	//==========================================================================
	//
	// Makes the frame buffer for the current texture active
	//
	//==========================================================================

	void FGLRenderBuffers::BindCurrentFB()
	{
#ifndef NO_RENDER_BUFFER
		rtStatsGLES.fboBinds++;
		mResolvePending = true;
		mSceneFB.Bind();
#endif
	}

	//==========================================================================
	//
	// Makes the screen frame buffer active
	//
	//==========================================================================

	void FGLRenderBuffers::BindOutputFB()
	{
		rtStatsGLES.fboBinds++;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	//==========================================================================
	//
	// Returns true if render buffers are supported and should be used
	//
	//==========================================================================

	bool FGLRenderBuffers::FailedCreate = false;




}  // namespace OpenGLRenderer
