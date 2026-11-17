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
EXTERN_CVAR(Bool, gl_msaa_angle_rtt)	// gl_renderbuffers.cpp

namespace OpenGLRenderer
{
	// the render target pool settings are shared with the GL renderer (gl_renderbuffers.cpp)
	EXTERN_CVAR(Int, gl_rtpool_budget_mb)
	EXTERN_CVAR(Int, gl_rtpool_maxage)
}

namespace OpenGLESRenderer
{

	//==========================================================================
	//
	// Render target pool
	//
	//==========================================================================

	namespace
	{
		struct PoolEntry
		{
			GLuint handle;
			bool renderbuffer;
			GLenum format;
			int width, height, samples;
			bool renderToTexture;
			size_t bytes;
			uint64_t released;	// frame of release
		};
		TArray<PoolEntry> FreeTargets;
		size_t FreeBytes = 0;
		uint64_t PoolFrame = 0;

		size_t TargetBytes(GLenum format, int width, int height, int samples, bool renderToTexture)
		{
			size_t bpp = 4;
			switch (format)
			{
			case GL_DEPTH_COMPONENT16:	bpp = 2; break;
			case GL_STENCIL_INDEX8:		bpp = 1; break;
			default: break;
			}
			// render-to-texture samples only exist in tile memory
			const size_t count = (samples > 1 && !renderToTexture) ? size_t(samples) : 1;
			if (width <= 0 || height <= 0) return 0;
			return size_t(width) * size_t(height) * bpp * count;
		}

		void TrackBytes(int64_t delta)
		{
			int64_t now = int64_t(rtStatsGLES.bytes) + delta;
			rtStatsGLES.bytes = now > 0 ? size_t(now) : 0;
			if (rtStatsGLES.bytes > rtStatsGLES.peakBytes) rtStatsGLES.peakBytes = rtStatsGLES.bytes;
		}

		bool TakeFromPool(bool renderbuffer, GLenum format, int width, int height, int samples, bool renderToTexture, GLuint &handle)
		{
			for (unsigned i = 0; i < FreeTargets.Size(); i++)
			{
				auto &e = FreeTargets[i];
				if (e.renderbuffer == renderbuffer && e.format == format && e.width == width && e.height == height &&
					e.samples == samples && e.renderToTexture == renderToTexture)
				{
					handle = e.handle;
					FreeBytes -= e.bytes;
					FreeTargets.Delete(i);
					rtStatsGLES.poolHits++;
					return true;
				}
			}
			rtStatsGLES.poolMisses++;
			return false;
		}

		void PutInPool(bool renderbuffer, GLuint handle, GLenum format, int width, int height, int samples, bool renderToTexture)
		{
			if (!handle) return;
			size_t bytes = TargetBytes(format, width, height, samples, renderToTexture);
			FreeTargets.Push({ handle, renderbuffer, format, width, height, samples, renderToTexture, bytes, PoolFrame });
			FreeBytes += bytes;
			GLESRenderTargetPool::Trim(false);
		}
	}

	GLuint GLESRenderTargetPool::AcquireTexture(int width, int height)
	{
		GLuint handle = 0;
		if (TakeFromPool(false, GL_RGBA, width, height, 0, false, handle)) return handle;
		glGenTextures(1, &handle);
		glBindTexture(GL_TEXTURE_2D, handle);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		TrackBytes((int64_t)TargetBytes(GL_RGBA, width, height, 0, false));
		rtStatsGLES.created++;
		return handle;
	}

	void GLESRenderTargetPool::ReleaseTexture(GLuint handle, int width, int height)
	{
		PutInPool(false, handle, GL_RGBA, width, height, 0, false);
	}

	GLuint GLESRenderTargetPool::AcquireRenderbuffer(GLenum format, int width, int height, int samples, bool renderToTexture)
	{
		typedef void (APIENTRY *RBSMS)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
		GLuint handle = 0;
		if (TakeFromPool(true, format, width, height, samples, renderToTexture, handle)) return handle;
		glGenRenderbuffers(1, &handle);
		glBindRenderbuffer(GL_RENDERBUFFER, handle);
		if (samples > 1 && renderToTexture)
			((RBSMS)gles.pRenderbufferStorageMultisampleRTT)(GL_RENDERBUFFER, samples, format, width, height);	// the extension's own entry point
		else if (samples > 1)
			((RBSMS)gles.pRenderbufferStorageMultisample)(GL_RENDERBUFFER, samples, format, width, height);	// core, or ANGLE/NV on GLES 2.0
		else
			glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
		TrackBytes((int64_t)TargetBytes(format, width, height, samples, renderToTexture));
		rtStatsGLES.created++;
		return handle;
	}

	void GLESRenderTargetPool::ReleaseRenderbuffer(GLuint handle, GLenum format, int width, int height, int samples, bool renderToTexture)
	{
		PutInPool(true, handle, format, width, height, samples, renderToTexture);
	}

	void GLESRenderTargetPool::Trim(bool all)
	{
		const size_t budget = size_t(std::max(0, (int)OpenGLRenderer::gl_rtpool_budget_mb)) << 20;
		const uint64_t maxAge = (uint64_t)std::max(1, (int)OpenGLRenderer::gl_rtpool_maxage);
		// oldest releases first
		for (unsigned i = 0; i < FreeTargets.Size();)
		{
			auto &e = FreeTargets[i];
			if (all || FreeBytes > budget || PoolFrame - e.released > maxAge)
			{
				if (e.renderbuffer) glDeleteRenderbuffers(1, &e.handle);
				else glDeleteTextures(1, &e.handle);
				FreeBytes -= e.bytes;
				TrackBytes(-(int64_t)e.bytes);
				rtStatsGLES.deleted++;
				FreeTargets.Delete(i);
			}
			else i++;
		}
	}

	void GLESRenderTargetPool::FrameEnd()
	{
		PoolFrame++;
		if (FreeTargets.Size()) Trim(false);
		rtStatsGLES.pooledFreeBytes = FreeBytes;
		// textures of post-process effects that were turned off
		PPResource::ReleaseIdleBackends();
	}

	uint64_t GLESRenderTargetPool::Frame()
	{
		return PoolFrame;
	}

	bool PPGLTextureBackend::Idle() const
	{
		return GLESRenderTargetPool::Frame() - LastUse > (uint64_t)std::max(1, (int)OpenGLRenderer::gl_rtpool_maxage);
	}

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
		ClearPipeline();

		DeleteTexture(mSceneTex);
		DeleteTexture(mDitherTexture);
	}

	void FGLRenderBuffers::ClearPipeline()
	{
		for (auto &fb : mPipelineFB)
			DeleteFrameBuffer(fb);
		DeleteRenderBuffer(mPipelineDepthStencilBuf);
		DeleteTexture(mPipelineTex1);
		mCurrentPipelineTexture = 0;
		mSceneFBCurrent = true;
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
		mSceneSamples = 0;
	}

	//==========================================================================
	//
	// MSAA limit of the scene. The tile-based path comes first: EXT/IMG
	// multisampled render-to-texture, GLES 2.0 extensions that a GLES 3.x
	// context has as well. The samples only exist in tile memory and are
	// resolved into the scene texture when a tile is written out, so its limit
	// is what the GPU renders without extra memory traffic. Without it, GLES 3.0
	// falls back to multisampled renderbuffers resolved by a blit; a GLES 2.0
	// context without it has no MSAA.
	//
	//==========================================================================

	// ANGLE on Vulkan lists EXT_multisampled_render_to_texture even where the Vulkan driver lacks
	// VK_EXT_multisampled_render_to_single_sampled, and then emulates it: each render pass resolves the samples
	// at its end and, when it starts by loading, first "unresolves" the texture back into a hidden multisampled
	// image with a full-screen draw. With the post-process, 2D and capture passes that is several full-screen
	// copies a frame - very slow even on a Snapdragon 8 Elite, while the native GLES driver resolves on chip.
	// Multisampled renderbuffers resolved by a blit are what ANGLE turns into a render pass resolve attachment,
	// so they are used there instead (gl_msaa_angle_rtt 1: its render-to-texture all the same).
static bool UseMSRTT()
	{
		if (!gles.msrtt) return false;
		if (gles.angle && gles.msaaRenderbuffers && !gl_msaa_angle_rtt) return false;
		return true;
	}

	// 4x at most, whatever the driver reports: on a tile-based GPU 4 samples per pixel fit the tile memory at
	// full tile size, 8x and 16x shrink the tiles and multiply the passes over the screen.
	int FGLRenderBuffers::MultisampleLimit()
	{
		const int MAX_SAMPLES = 4;
		if (UseMSRTT()) return std::min(gles.msrttMaxSamples, MAX_SAMPLES);
		if (gles.msaaRenderbuffers) return std::min(gles.maxSamples, MAX_SAMPLES);
		return 0;
	}

	const char *FGLRenderBuffers::MultisamplePath()
	{
		if (UseMSRTT() && gles.msrtt == 1) return "EXT_multisampled_render_to_texture, resolved in tile memory";
		if (UseMSRTT() && gles.msrtt == 2) return "IMG_multisampled_render_to_texture, resolved in tile memory";
		if (gles.msaaRenderbuffers && gles.angle && gles.msrtt) return "multisampled renderbuffers, blit resolve (ANGLE: its render-to-texture is emulated)";
		if (gles.msaaRenderbuffers) return "multisampled renderbuffers, blit resolve";
		return gles.esVersion > 0 ? "no multisampled render-to-texture extension" : "no multisampled render buffers";
	}

	void FGLRenderBuffers::DeleteTexture(PPGLTexture& tex)
	{
		if (tex.handle != 0)
		{
			if (tex.pooled)
			{
				GLESRenderTargetPool::ReleaseTexture(tex.handle, tex.Width, tex.Height);
			}
			else
			{
				glDeleteTextures(1, &tex.handle);
				rtStatsGLES.deleted++;
				TrackBytes(-(int64_t)TargetBytes(GL_RGBA, tex.Width, tex.Height, 0, false));
			}
		}
		tex.handle = 0;
		tex.pooled = false;
	}

	void FGLRenderBuffers::DeleteRenderBuffer(PPGLRenderBuffer& buf)
	{
		if (buf.handle != 0)
			GLESRenderTargetPool::ReleaseRenderbuffer(buf.handle, buf.format, buf.width, buf.height, buf.samples, buf.renderToTexture);
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
		int samples = V_MultisampleCount(gl_multisample, MultisampleLimit());

		const bool resized = width != mWidth || height != mHeight;
		if (resized)
		{
			// the frame buffer references the old texture: release both before creating the new set
			ClearScene();
			CreatePipeline(width, height);
			// targets of the old size are not coming back: free them now instead of keeping two sets
			GLESRenderTargetPool::Trim(true);
		}

		if (resized || samples != mSamples)
		{
			CreateScene(width, height, samples);
			rtStatsGLES.msaaSamples = mSceneSamples;
			rtStatsGLES.msaaPath = mMSAAMode == MSAA_RenderToTexture ? MultisamplePath() : "multisampled renderbuffers, blit resolve";
		}
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
		if (samples > 1 && UseMSRTT() && CreateSceneRTT(width, height, V_MultisampleCount(samples, gles.msrttMaxSamples))) return;
		// multisampled renderbuffers can have a lower limit than render-to-texture
		if (gles.msaaRenderbuffers) samples = V_MultisampleCount(samples, gles.maxSamples);
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
		ClearPipeline();	// the post-process frame buffers reference the scene texture
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
		mSceneSamples = samples;
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
		mSceneSamples = samples;
		return true;
	}

	void FGLRenderBuffers::ResolveScene()
	{
		// !mSceneFBCurrent: a post-process effect wrote the scene texture, the multisampled
		// buffers hold an older image
		if (mMSAAMode != MSAA_Renderbuffers || !mResolvePending || !mSceneFBCurrent) return;
		GLint fb = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mSceneFB.handle);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mResolveFB.handle);
		typedef void (APIENTRY *BLITFB)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
		((BLITFB)gles.pBlitFramebuffer)(0, 0, mWidth, mHeight, 0, 0, mWidth, mHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);	// core, or ANGLE/NV
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
		mResolvePending = false;
		rtStatsGLES.resolves++;
	}

	void FGLRenderBuffers::BindReadableFB()
	{
		const GLuint fb = ReadableFB();
		rtStatsGLES.fboBinds++;
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
	}

	// The single-sampled frame buffer of the current image
	GLuint FGLRenderBuffers::ReadableFB()
	{
		if (mCurrentPipelineTexture == 0 && mSceneFBCurrent)
		{
			ResolveScene();
			return mMSAAMode == MSAA_Renderbuffers ? mResolveFB.handle : mSceneFB.handle;
		}
		return PipelineFB(mCurrentPipelineTexture).handle;
	}

	//==========================================================================
	//
	// Post-process pipeline frame buffers: single-sampled, with a depth/stencil
	// buffer of their own for the HUD models and the 2D drawn after the effects
	// (the models clear it first). Made when first needed. Creating one keeps
	// the texture and renderbuffer bindings an effect has set up already, and
	// does not clear anything (texture 0 holds the scene).
	//
	//==========================================================================

	PPGLFrameBuffer &FGLRenderBuffers::PipelineFB(int index)
	{
		auto &fb = mPipelineFB[index];
		if (!fb)
		{
			GLint activeTex = 0, textureBinding = 0, renderbuffer = 0;
			glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
			glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);

			if (index == 1 && !mPipelineTex1)
				mPipelineTex1 = Create2DTexture("PipelineTexture", GL_RGBA, mWidth, mHeight);
			if (!mPipelineDepthStencilBuf)
				mPipelineDepthStencilBuf = CreateRenderBuffer("PipelineDepthStencil", GL_DEPTH24_STENCIL8, mWidth, mHeight);

			glGenFramebuffers(1, &fb.handle);
			glBindFramebuffer(GL_FRAMEBUFFER, fb.handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, index == 0 ? mSceneTex.handle : mPipelineTex1.handle, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mPipelineDepthStencilBuf.handle);
			CheckFrameBufferCompleteness();

			glBindTexture(GL_TEXTURE_2D, textureBinding);
			glActiveTexture(activeTex);
			glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		}
		return fb;
	}

	//==========================================================================
	//
	// Post-process textures (PPTexture): the formats the effects ask for, as
	// far as this GLES 3.0 device can render to them. Not pooled.
	//
	//==========================================================================

	PPGLTexture FGLRenderBuffers::CreatePPTexture(GLuint format, int width, int height, const void *data)
	{
		PPGLTexture tex;
		tex.Width = width;
		tex.Height = height;

		if (!data)
		{
			switch (format)
			{
			case GL_R16F:	if (!gles.colorbuffer16f) format = GL_R8; break;
			case GL_RG16F:	if (!gles.colorbuffer16f) format = GL_RG8; break;
			case GL_RGBA16F:	if (!gles.colorbuffer16f) format = GL_RGBA8; break;
			// 32-bit float render targets cannot be blended (EXT_float_blend): half float is plenty here
			case GL_R32F:	format = gles.colorbuffer16f ? GL_R16F : GL_R8; break;
			case GL_RG32F:	format = gles.colorbuffer16f ? GL_RG16F : GL_RG8; break;
			case GL_R11F_G11F_B10F:	if (!gles.colorbuffer32f) format = gles.colorbuffer16f ? GL_RGBA16F : GL_RGBA8; break;
			default: break;
			}
		}

		GLenum dataformat = GL_RGBA, datatype = GL_UNSIGNED_BYTE;
		TArray<int8_t> snorm8;
		switch (format)
		{
		default:
		case GL_RGBA8:			format = GL_RGBA8; dataformat = GL_RGBA; datatype = GL_UNSIGNED_BYTE; break;
		case GL_RG8:			dataformat = GL_RG; datatype = GL_UNSIGNED_BYTE; break;
		case GL_R8:				dataformat = GL_RED; datatype = GL_UNSIGNED_BYTE; break;
		case GL_RGBA16F:		dataformat = GL_RGBA; datatype = GL_FLOAT; break;
		case GL_RG16F:			dataformat = GL_RG; datatype = GL_FLOAT; break;
		case GL_R16F:			dataformat = GL_RED; datatype = GL_FLOAT; break;
		case GL_R32F:			dataformat = GL_RED; datatype = GL_FLOAT; break;
		case GL_RG32F:			dataformat = GL_RG; datatype = GL_FLOAT; break;
		case GL_R11F_G11F_B10F:	dataformat = GL_RGB; datatype = GL_FLOAT; break;
		case GL_RGBA16_SNORM:
			// GLES has no 16-bit normalized formats: 8 bits, every component converted into a copy
			format = GL_RGBA8_SNORM;
			dataformat = GL_RGBA;
			datatype = GL_BYTE;
			if (data)
			{
				const unsigned n = unsigned(width) * unsigned(height) * 4;
				snorm8.Resize(n);
				for (unsigned i = 0; i < n; i++)
					snorm8[i] = int8_t(((const int16_t*)data)[i] / 256);
				data = snorm8.Data();
			}
			break;
		}

		const bool float32 = format == GL_R32F || format == GL_RG32F;
		tex.nearestOnly = float32 && !gles.floatlinear;
		const GLint filter = tex.nearestOnly ? GL_NEAREST : GL_LINEAR;

		glGenTextures(1, &tex.handle);
		glBindTexture(GL_TEXTURE_2D, tex.handle);
		if (!data && glTexStorage2D)
			glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);	// render targets are never respecified
		else
			glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, dataformat, datatype, data);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return tex;
	}

	void FGLRenderBuffers::Discard(int count, const GLenum *attachments)
	{
		DiscardAttachments(count, attachments);
	}

	void FGLRenderBuffers::DiscardAttachments(int count, const GLenum *attachments)
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
		// the post-process pipeline's depth/stencil (HUD models) as well
		for (auto &pfb : mPipelineFB)
		{
			if (!pfb) continue;
			glBindFramebuffer(GL_FRAMEBUFFER, pfb.handle);
			Discard(2, attachments);
		}
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
		if (!data)
		{
			// a render target: from the pool (its contents are cleared by the frame buffer setup)
			tex.handle = GLESRenderTargetPool::AcquireTexture(width, height);
			tex.pooled = true;
			glBindTexture(GL_TEXTURE_2D, tex.handle);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			return tex;
		}
		rtStatsGLES.created++;
		TrackBytes((int64_t)TargetBytes(GL_RGBA, width, height, 0, false));
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
		return CreateRenderBuffer(name, format, width, height, 0, false);
	}

	PPGLRenderBuffer FGLRenderBuffers::CreateRenderBuffer(const char* name, GLuint format, int width, int height, int samples, bool renderToTexture)
	{
		PPGLRenderBuffer buf;
		buf.handle = GLESRenderTargetPool::AcquireRenderbuffer(format, width, height, samples, renderToTexture);
		buf.format = format;
		buf.width = width;
		buf.height = height;
		buf.samples = samples;
		buf.renderToTexture = renderToTexture;
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
		// a new scene: the pipeline starts over at the scene texture
		mCurrentPipelineTexture = 0;
		mSceneFBCurrent = true;
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
		if (mCurrentPipelineTexture == 0)
		{
			ResolveScene();
			mSceneTex.Bind(index, filter, wrap);
		}
		else
		{
			mPipelineTex1.Bind(index, filter, wrap);
		}
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
		if (mCurrentPipelineTexture == 0 && mSceneFBCurrent)
		{
			mResolvePending = true;
			mSceneFB.Bind();
		}
		else
		{
			PipelineFB(mCurrentPipelineTexture).Bind();
		}
#endif
	}

	//==========================================================================
	//
	// Post-process pipeline: the frame buffer of the next texture, and making
	// that the current one once an effect wrote it
	//
	//==========================================================================

	void FGLRenderBuffers::BindNextFB()
	{
		const int out = 1 - mCurrentPipelineTexture;
		auto &fb = PipelineFB(out);
		// texture 0 is now written directly: the scene frame buffer no longer holds its image
		if (out == 0) mSceneFBCurrent = false;
		rtStatsGLES.fboBinds++;
		fb.Bind();
	}

	void FGLRenderBuffers::NextTexture()
	{
		mCurrentPipelineTexture = 1 - mCurrentPipelineTexture;
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

	//==========================================================================
	//
	// Post-process effects (GLES 3.0 mode)
	//
	//==========================================================================

	PPGLTextureBackend *GLPPRenderState::GetGLTexture(PPTexture *texture)
	{
		if (!texture->Backend)
		{
			FGLPostProcessState savedState;

			auto backend = std::make_unique<PPGLTextureBackend>();

			GLuint glformat;
			switch (texture->Format)
			{
			default:
			case PixelFormat::Rgba8: glformat = GL_RGBA8; break;
			// the pipeline textures are RGBA8: glCopyTexSubImage2D cannot copy them into a float texture
			// (the custom shaders' last input), and it would hold nothing more than their 8 bits
			case PixelFormat::Rgba16f: glformat = GL_RGBA8; break;
			case PixelFormat::R32f: glformat = GL_R32F; break;
			case PixelFormat::Rg16f: glformat = GL_RG16F; break;
			case PixelFormat::Rgba16_snorm: glformat = GL_RGBA16_SNORM; break;
			case PixelFormat::Rgb11f: glformat = GL_R11F_G11F_B10F; break;
			case PixelFormat::R16f: glformat = GL_R16F; break;
			}

			backend->Tex = buffers->CreatePPTexture(glformat, texture->Width, texture->Height, texture->Data.get());
			texture->Backend = std::move(backend);
		}
		auto glbackend = static_cast<PPGLTextureBackend*>(texture->Backend.get());
		glbackend->LastUse = GLESRenderTargetPool::Frame();
		return glbackend;
	}

	static const char *PPUniformTypeStr(UniformType type)
	{
		switch (type)
		{
		default:
		case UniformType::Int: return "int";
		case UniformType::UInt: return "uint";
		case UniformType::Float: return "float";
		case UniformType::Vec2: return "vec2";
		case UniformType::Vec3: return "vec3";
		case UniformType::Vec4: return "vec4";
		case UniformType::IVec2: return "ivec2";
		case UniformType::IVec3: return "ivec3";
		case UniformType::IVec4: return "ivec4";
		case UniformType::UVec2: return "uvec2";
		case UniformType::UVec3: return "uvec3";
		case UniformType::UVec4: return "uvec4";
		case UniformType::Mat4: return "mat4";
		}
	}

	// The effects' shaders are the GL renderer's desktop GLSL ones. Their uniforms are plain
	// uniforms here instead of a uniform block: the GLES renderer has no uniform buffers.
	FShaderProgram *GLPPRenderState::GetGLShader(PPShader *shader)
	{
		if (!shader->Backend)
		{
			auto glshader = std::make_unique<FShaderProgram>();
			glshader->SetDesktopGLSL(true);

			FString prolog;
			for (const auto &field : shader->Uniforms)
				prolog.AppendFormat("uniform %s %s;\n", PPUniformTypeStr(field.Type), field.Name);
			prolog += shader->Defines;
			prolog += "uniform float InputTimeDelta;\n";
			prolog += "uniform float InputTime;\n";
			prolog += "uniform float InputTimeGame;\n";

			glshader->Compile(FShaderProgram::Vertex, shader->VertexShader.GetChars(), "", shader->Version);
			glshader->Compile(FShaderProgram::Fragment, shader->FragmentShader.GetChars(), prolog.GetChars(), shader->Version);
			glshader->Link(shader->FragmentShader.GetChars());
			glshader->SetPPUniforms(shader->Uniforms);
			shader->Backend = std::move(glshader);
		}
		return static_cast<FShaderProgram*>(shader->Backend.get());
	}

	void GLPPRenderState::Draw()
	{
		FGLPostProcessState savedState;

		auto shader = GetGLShader(Shader);
		if (shader->Failed())
		{
			// The effect is left out: the pipeline goes on with its current texture.
			return;
		}

		// Bind input textures
		for (unsigned int index = 0; index < Textures.Size(); index++)
		{
			savedState.SaveTextureBindings(index + 1);

			const PPTextureInput &input = Textures[index];
			int filter = (input.Filter == PPFilterMode::Nearest) ? GL_NEAREST : GL_LINEAR;
			int wrap = (input.Wrap == PPWrapMode::Clamp) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

			switch (input.Type)
			{
			default:
			case PPTextureType::CurrentPipelineTexture:
			case PPTextureType::SceneColor:
				buffers->BindCurrentTexture(index, filter, wrap);
				break;

			case PPTextureType::NextPipelineTexture:
				I_FatalError("PPTextureType::NextPipelineTexture not allowed as input\n");
				break;

			case PPTextureType::PPTexture:
				GetGLTexture(input.Texture)->Tex.Bind(index, filter, wrap);
				break;

			case PPTextureType::SceneFog:
			case PPTextureType::SceneNormal:
			case PPTextureType::SceneDepth:
				// no G-buffer (SSAO) on this renderer
				glActiveTexture(GL_TEXTURE0 + index);
				glBindTexture(GL_TEXTURE_2D, 0);
				glActiveTexture(GL_TEXTURE0);
				break;
			}
		}

		// Set render target
		switch (Output.Type)
		{
		default:
			I_FatalError("Unsupported postprocess output type\n");
			break;

		case PPTextureType::CurrentPipelineTexture:
		case PPTextureType::SceneColor:
			buffers->BindCurrentFB();
			break;

		case PPTextureType::NextPipelineTexture:
			buffers->BindNextFB();
			break;

		case PPTextureType::PPTexture:
		{
			auto backend = GetGLTexture(Output.Texture);
			if (backend->FB)
				backend->FB.Bind();
			else
				backend->FB = buffers->CreateFrameBuffer("PPTextureFB", backend->Tex);
			break;
		}
		}

		// Set blend mode
		const bool opaque = BlendMode.BlendOp == STYLEOP_Add && BlendMode.SrcAlpha == STYLEALPHA_One && BlendMode.DestAlpha == STYLEALPHA_Zero && BlendMode.Flags == 0;
		if (opaque)
		{
			glDisable(GL_BLEND);
		}
		else
		{
			// To do: support all the modes
			glEnable(GL_BLEND);
			glBlendEquation(GL_FUNC_ADD);
			if (BlendMode.SrcAlpha == STYLEALPHA_One && BlendMode.DestAlpha == STYLEALPHA_One)
				glBlendFunc(GL_ONE, GL_ONE);
			else
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}

		// Tile-based GPUs: a pass that writes every pixel of its target without blending needs none
		// of the old contents loaded into tile memory, nor the pipeline depth/stencil.
		if (opaque && Viewport.left == 0 && Viewport.top == 0 && (Output.Type == PPTextureType::NextPipelineTexture || Output.Type == PPTextureType::PPTexture))
		{
			const bool pipeline = Output.Type == PPTextureType::NextPipelineTexture;
			const int width = pipeline ? buffers->GetWidth() : Output.Texture->Width;
			const int height = pipeline ? buffers->GetHeight() : Output.Texture->Height;
			bool readsOutput = false;
			for (auto &input : Textures)
			{
				if (!pipeline && input.Type == PPTextureType::PPTexture && input.Texture == Output.Texture) readsOutput = true;
			}
			if (Viewport.width == width && Viewport.height == height && !readsOutput)
			{
				static const GLenum attachments[] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
				FGLRenderBuffers::DiscardAttachments(pipeline ? 2 : 1, attachments);
			}
		}

		// Setup viewport
		glViewport(Viewport.left, Viewport.top, Viewport.width, Viewport.height);

		// Set shader and uniforms
		shader->Bind();
		shader->ApplyPPUniforms(Uniforms.Data);
		if (shader->mTimeDeltaLoc != -1) glUniform1f(shader->mTimeDeltaLoc, TimeDelta);
		if (shader->mTimeLoc != -1) glUniform1f(shader->mTimeLoc, Time);
		if (shader->mTimeGameLoc != -1) glUniform1f(shader->mTimeGameLoc, TimeGame);

		// Draw the screen quad
		GLRenderer->RenderScreenQuad();

		// Advance to next PP texture if our output was sent there
		if (Output.Type == PPTextureType::NextPipelineTexture)
			buffers->NextTexture();

		glViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);
	}

	// The custom shaders' last input: a copy of the current image
	void GLPPRenderState::CopyToTexture(PPTexture *dst)
	{
		PPGLTextureBackend *backend = GetGLTexture(dst);

		GLint fb = 0, activeTex = 0, textureBinding = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		const GLuint srcFB = buffers->ReadableFB();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFB);

		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
		glBindTexture(GL_TEXTURE_2D, backend->Tex.handle);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, dst->Width, dst->Height);
		glBindTexture(GL_TEXTURE_2D, textureBinding);
		glActiveTexture(activeTex);

		glBindFramebuffer(GL_FRAMEBUFFER, fb);
	}

}  // namespace OpenGLRenderer
