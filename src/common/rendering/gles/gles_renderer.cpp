/*
** gles_renderer.cpp
**
** Renderer interface
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2020 Christoph Oelckers
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

#include "gles_system.h"
#include "printf.h"
#include "files.h"
#include "v_video.h"
#include "m_png.h"
#include "filesystem.h"
#include "i_time.h"
#include "cmdlib.h"
#include "version.h"
#include "gles_framebuffer.h"
#include "hw_cvars.h"
#include "gles_renderer.h"
#include "gles_hwtexture.h"
#include "gles_renderstate.h"
#include "gles_samplers.h"
#include "gles_renderbuffers.h"
#include "gles_shaderprogram.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "r_videoscale.h"
#include "model.h"
#include "gles_postprocessstate.h"
#include "gles_buffers.h"
#include "texturemanager.h"

EXTERN_CVAR(Int, screenblocks)

namespace OpenGLESRenderer
{

//===========================================================================
//
// Renderer interface
//
//===========================================================================

//-----------------------------------------------------------------------------
//
// Initialize
//
//-----------------------------------------------------------------------------

FGLRenderer::FGLRenderer(OpenGLFrameBuffer *fb)
{
	framebuffer = fb;
}

void FGLRenderer::Initialize(int width, int height)
{
	mScreenBuffers = new FGLRenderBuffers();
	mBuffers = mScreenBuffers;
	mPresentShader = new FPresentShader();

	mFBID = 0;
	mOldFBID = 0;

	mShaderManager = new FShaderManager;
	mSamplerManager = new FSamplerManager;
}

FGLRenderer::~FGLRenderer()
{
	FlushModels();
	TexMan.FlushAll();
	if (mShaderManager != nullptr) delete mShaderManager;
	if (mFBID != 0) glDeleteFramebuffers(1, &mFBID);
	DeleteLightmaps();

	if (mSaveBuffers) delete mSaveBuffers;
	if (mScreenBuffers) delete mScreenBuffers;
	mBuffers = mSaveBuffers = mScreenBuffers = nullptr;
	if (mPresentShader) delete mPresentShader;
}

//===========================================================================
//
//
//
//===========================================================================

//===========================================================================
//
// Selaco's master lightmaps: RGBA8 render targets (P_RenderLightGroups draws the base lightmaps and
// the light groups into them), linear filtering, no mipmaps
//
//===========================================================================

void FGLRenderer::CreateLightmaps(int width, int height, int count)
{
	if (!gles.lightmapArrays && count > gles.lightmapUnitCount)
	{
		Printf(TEXTCOLOR_ORANGE "This level has %d master lightmaps, OpenGL ES 2.0 can show %d of them\n", count, gles.lightmapUnitCount);
		count = gles.lightmapUnitCount;
	}
	if (width == mLightMapWidth && height == mLightMapHeight && count == mLightMapCount)
		return;	// (the next level's are drawn over these)
	DeleteLightmaps();
	if (count <= 0 || width <= 0 || height <= 0)
		return;

	GLint activeTex = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0 + gles.lightmapUnit);
	if (gles.lightmapArrays)
	{
		glGenTextures(1, &mLightMapArray);
		glBindTexture(GL_TEXTURE_2D_ARRAY, mLightMapArray);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height, count, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		mLightMapArrayBound = true;
	}
	else
	{
		glGenTextures(count, mLightMapTextures);
		for (int i = 0; i < count; i++)
		{
			glBindTexture(GL_TEXTURE_2D, mLightMapTextures[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		FHardwareTexture::lastbound[gles.lightmapUnit] = 0;
	}
	glActiveTexture(activeTex);

	mLightMapWidth = width;
	mLightMapHeight = height;
	mLightMapCount = count;
}

void FGLRenderer::DeleteLightmaps()
{
	if (mLightMapArray)
		glDeleteTextures(1, &mLightMapArray);
	for (auto &tex : mLightMapTextures)
	{
		if (tex)
		{
			for (int unit = 0; unit < IHardwareTexture::MAX_TEXTURES; unit++)
			{
				if (FHardwareTexture::lastbound[unit] == tex)
					FHardwareTexture::lastbound[unit] = 0;
			}
			glDeleteTextures(1, &tex);
		}
		tex = 0;
	}
	mLightMapArray = 0;
	mLightMapArrayBound = false;
	mLightMapWidth = mLightMapHeight = mLightMapCount = 0;
}

void FGLRenderer::BindLightmaps()
{
	if (gles.lightmapArrays)
	{
		// (the material binding only unbinds GL_TEXTURE_2D: the array stays bound once it is)
		if (mLightMapArrayBound)
			return;
		glActiveTexture(GL_TEXTURE0 + gles.lightmapUnit);
		glBindTexture(GL_TEXTURE_2D_ARRAY, mLightMapArray);
		glActiveTexture(GL_TEXTURE0);
		mLightMapArrayBound = true;
		return;
	}
	for (int i = 0; i < mLightMapCount; i++)
	{
		const int unit = gles.lightmapUnit + i;
		if (FHardwareTexture::lastbound[unit] == mLightMapTextures[i])
			continue;
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, mLightMapTextures[i]);
		FHardwareTexture::lastbound[unit] = mLightMapTextures[i];
		glActiveTexture(GL_TEXTURE0);
	}
}

bool FGLRenderer::StartOffscreen()
{
	if (mFBID == 0)
		glGenFramebuffers(1, &mFBID);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mOldFBID);
	glBindFramebuffer(GL_FRAMEBUFFER, mFBID);
	return true;
}

//===========================================================================
//
//
//
//===========================================================================

void FGLRenderer::EndOffscreen()
{
	if (mOffscreenDepth)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, mFBID);
		ReleaseOffscreenDepth();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, mOldFBID);
}

//===========================================================================
//
// Camera views: one depth/stencil buffer per size from the render target
// pool, shared by all camera textures of that size (they render one after
// another and each view clears it first). After the view it is discarded -
// a tiler does not write it back - and returned.
//
//===========================================================================

void FGLRenderer::AttachOffscreenDepth(int width, int height)
{
	ReleaseOffscreenDepth();
	const bool packed = gles.depthStencilAvailable;
	mOffscreenDepth = GLESRenderTargetPool::AcquireRenderbuffer(packed ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, width, height, 0, false);
	mOffscreenStencil = packed ? 0 : GLESRenderTargetPool::AcquireRenderbuffer(GL_STENCIL_INDEX8, width, height, 0, false);
	mOffscreenDepthWidth = width;
	mOffscreenDepthHeight = height;
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mOffscreenDepth);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, packed ? mOffscreenDepth : mOffscreenStencil);
}

void FGLRenderer::ReleaseOffscreenDepth()
{
	if (!mOffscreenDepth) return;
	static const GLenum attachments[] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
	FGLRenderBuffers::DiscardAttachments(2, attachments);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
	const bool packed = mOffscreenStencil == 0;
	GLESRenderTargetPool::ReleaseRenderbuffer(mOffscreenDepth, packed ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, mOffscreenDepthWidth, mOffscreenDepthHeight, 0, false);
	if (mOffscreenStencil)
		GLESRenderTargetPool::ReleaseRenderbuffer(mOffscreenStencil, GL_STENCIL_INDEX8, mOffscreenDepthWidth, mOffscreenDepthHeight, 0, false);
	mOffscreenDepth = mOffscreenStencil = 0;
}

//===========================================================================
//
//
//
//===========================================================================

void FGLRenderer::BindToFrameBuffer(FTexture *tex)
{
	auto BaseLayer = static_cast<FHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	// must create the hardware texture first
	BaseLayer->BindOrCreate(tex, 0, 0, 0, 0);
	FHardwareTexture::Unbind(0);
	gl_RenderState.ClearLastMaterial();
	BaseLayer->BindToFrameBuffer(tex->GetWidth(), tex->GetHeight());
	AttachOffscreenDepth(FHardwareTexture::GetTexDimension(tex->GetWidth()), FHardwareTexture::GetTexDimension(tex->GetHeight()));
}

//===========================================================================
//
//
//
//===========================================================================

void FGLRenderer::BeginFrame()
{
	mScreenBuffers->Setup(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
}

}
