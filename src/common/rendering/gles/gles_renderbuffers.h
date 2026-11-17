/*
** gles_renderbuffers.h
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

#pragma once

#include "hwrenderer/postprocessing/hw_postprocess.h"

namespace OpenGLESRenderer
{

class FGLRenderBuffers;

class PPGLTexture
{
public:
	void Bind(int index, int filter = GL_NEAREST, int wrap = GL_CLAMP_TO_EDGE)
	{
		if (nearestOnly) filter = GL_NEAREST;	// a linear filter would make it incomplete (sampled as black)
		glActiveTexture(GL_TEXTURE0 + index);
		glBindTexture(GL_TEXTURE_2D, handle);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	}

	int Width = -1;
	int Height = -1;

	explicit operator bool() const { return handle != 0; }

private:
	GLuint handle = 0;
	bool pooled = false;	// a render target from GLESRenderTargetPool
	bool nearestOnly = false;	// 32-bit float without OES_texture_float_linear
	GLenum internalFormat = GL_RGBA;	// what it was made with (the pool hands it out again for the same)

	friend class FGLRenderBuffers;
	friend class PPGLTextureBackend;
	friend class GLPPRenderState;
};

class PPGLFrameBuffer
{
public:
	void Bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, handle);
	}

	explicit operator bool() const { return handle != 0; }

private:
	GLuint handle = 0;

	friend class FGLRenderBuffers;
	friend class PPGLTextureBackend;
	friend class GLPPRenderState;
};

class PPGLRenderBuffer
{
private:
	GLuint handle = 0;
	// what it was created as, to hand it back to GLESRenderTargetPool
	GLenum format = 0;
	int width = 0, height = 0, samples = 0;
	bool renderToTexture = false;

	explicit operator bool() const { return handle != 0; }

	friend class FGLRenderBuffers;
};

// Pool of render targets - textures without initial data (the scene and pipeline ones, and those of
// the post-process effects in every format they use) and renderbuffers, as the GL renderer has it. A
// released one is handed out again for the same size/format/sample count; unused ones are freed after
// gl_rtpool_maxage frames, or oldest first while the pool holds more than gl_rtpool_budget_mb.
class GLESRenderTargetPool
{
public:
	// GL_RGBA: unsized RGBA8 (glTexImage2D, GLES 2.0 as well), otherwise a sized format (immutable storage)
	static GLuint AcquireTexture(GLenum format, int width, int height);
	static void ReleaseTexture(GLuint handle, GLenum format, int width, int height);
	// A texture made with initial data (not pooled): deleted, its memory no longer counted
	static void DeleteTexture(GLuint handle, GLenum format, int width, int height);
	static GLuint AcquireRenderbuffer(GLenum format, int width, int height, int samples, bool renderToTexture);
	static void ReleaseRenderbuffer(GLuint handle, GLenum format, int width, int height, int samples, bool renderToTexture);
	static void FrameEnd();
	static void Trim(bool all);
	static uint64_t Frame();
};

// A texture of the post-process effects (PPTexture) and its frame buffer. A render target (no
// initial data) comes from GLESRenderTargetPool and goes back to it when the effect has not used it
// for gl_rtpool_maxage frames (turned off) or its size changes.
class PPGLTextureBackend : public PPTextureBackend
{
public:
	~PPGLTextureBackend();

	bool Idle() const override;

	PPGLTexture Tex;
	PPGLFrameBuffer FB;
	uint64_t LastUse = 0;	// GLESRenderTargetPool::Frame() of the last use
};

class FShaderProgram;

// The post-process effects (hw_postprocess: bloom, tonemap, lens, FXAA, the mods' custom shaders)
// on the GLES 3.0 path. Their shaders are the desktop GLSL ones of the GL renderer, converted to
// GLSL ES 3.00 (FShaderProgram::SetDesktopGLSL).
class GLPPRenderState : public PPRenderState
{
public:
	GLPPRenderState(FGLRenderBuffers *buffers) : buffers(buffers) { }

	void PushGroup(const FString &name) override {}
	void PopGroup() override {}
	void Draw() override;
	void CopyToTexture(PPTexture *dst) override;

private:
	PPGLTextureBackend *GetGLTexture(PPTexture *texture);
	FShaderProgram *GetGLShader(PPShader *shader);

	FGLRenderBuffers *buffers;
};

// Post-process effects on (gles_postprocess, GLES 3.0 mode only)
bool UsePostProcess();


class FGLRenderBuffers
{
public:
	FGLRenderBuffers();
	~FGLRenderBuffers();

	void Setup(int width, int height, int sceneWidth, int sceneHeight);

	void BindSceneFB(bool sceneData);


	void BindCurrentTexture(int index, int filter = GL_NEAREST, int wrap = GL_CLAMP_TO_EDGE);
	void BindCurrentFB();
	void BindNextFB();
	void NextTexture();


	void BindOutputFB();

	void BindDitherTexture(int texunit);

	// Multisampled scene (renderbuffer path): the color is resolved into the scene texture
	// before it is read; render-to-texture resolves inline.
	void ResolveScene();
	// Single-sampled frame buffer holding the current image (source of glCopyTexSubImage2D).
	void BindReadableFB();
	// After the frame's last draw: the scene depth/stencil need not be written back.
	void InvalidateSceneDepth();
	// The pipeline frame buffers get their depth/stencil buffer when depth is first cleared into them
	// (HUD models after the effects) or the stencil test is turned on while one is bound (2D with
	// stencil): the post-process passes and plain 2D never use it.
	void EnsurePipelineDepth();

	int GetWidth() const { return mWidth; }
	int GetHeight() const { return mHeight; }

	// glInvalidateFramebuffer (GLES 3.0) or glDiscardFramebufferEXT on the bound frame buffer.
	static void DiscardAttachments(int count, const GLenum *attachments);

	int GetSceneWidth() const { return mSceneWidth; }
	int GetSceneHeight() const { return mSceneHeight; }

	// Highest MSAA sample count offered for the scene (see the definition).
	static int MultisampleLimit();
	static const char *MultisamplePath();

private:
	void ClearScene();

	void CreateScene(int width, int height, int samples);
	PPGLRenderBuffer CreateRenderBuffer(const char *name, GLuint format, int width, int height, int samples, bool renderToTexture);
	bool CreateSceneRTT(int width, int height, int samples);
	bool CreateSceneMultisample(int width, int height, int samples);
	void Discard(int count, const GLenum *attachments);
	void CreatePipeline(int width, int height);
	void ClearPipeline();
	PPGLFrameBuffer &PipelineFB(int index);
	GLuint ReadableFB();
	PPGLTexture CreatePPTexture(GLuint format, int width, int height, const void *data);

	PPGLTexture Create2DTexture(const char *name, GLuint format, int width, int height, const void *data = nullptr);
	PPGLRenderBuffer CreateRenderBuffer(const char *name, GLuint format, int width, int height);
	PPGLFrameBuffer CreateFrameBuffer(const char *name, PPGLTexture colorbuffer);
	PPGLFrameBuffer CreateFrameBuffer(const char *name, PPGLTexture colorbuffer, PPGLRenderBuffer depthstencil, PPGLRenderBuffer stencil);
	PPGLFrameBuffer CreateFrameBuffer(const char *name, PPGLRenderBuffer colorbuffer, PPGLRenderBuffer depthstencil, PPGLRenderBuffer stencil);
	PPGLFrameBuffer CreateFrameBuffer(const char *name, PPGLTexture colorbuffer0, PPGLTexture colorbuffer1, PPGLTexture colorbuffer2, PPGLTexture depthstencil, PPGLRenderBuffer stencil);
	bool CheckFrameBufferCompleteness();
	void ClearFrameBuffer(bool stencil, bool depth);
	void DeleteTexture(PPGLTexture &handle);
	void DeleteRenderBuffer(PPGLRenderBuffer &handle);
	void DeleteFrameBuffer(PPGLFrameBuffer &handle);

	int mWidth = 0;
	int mHeight = 0;
	int mSceneWidth = 0;
	int mSceneHeight = 0;

	// Buffers for the scene
	PPGLTexture mSceneDepthStencilTex;
	PPGLTexture mSceneTex;
	PPGLRenderBuffer mSceneDepthStencilBuf;
	PPGLRenderBuffer mSceneStencilBuf; // This is only use when combined depth-stencil is not avaliable
	PPGLFrameBuffer mSceneFB;
	bool mSceneUsesTextures = false;

	enum EMSAAMode { MSAA_None, MSAA_RenderToTexture, MSAA_Renderbuffers };
	EMSAAMode mMSAAMode = MSAA_None;
	int mSamples = 0;		// requested count the scene buffers were created for
	int mSceneSamples = 0;	// count they actually have (0: single-sampled)
	PPGLRenderBuffer mSceneColorMSBuf;
	PPGLFrameBuffer mResolveFB;
	bool mResolvePending = false;

	PPGLTexture mDitherTexture;

	// Post-process pipeline. Texture 0 is mSceneTex. While mSceneFBCurrent, the scene frame buffer
	// still holds texture 0's image (the HUD and 2D go on drawing into it, in the scene's render
	// pass); once an effect wrote texture 0, or the multisampled scene was resolved for the effects,
	// texture 0 is drawn into through its own single-sampled frame buffer.
	PPGLTexture mPipelineTex1;
	PPGLFrameBuffer mPipelineFB[2];
	PPGLRenderBuffer mPipelineDepthStencilBuf;	// for HUD models after the effects: made when first cleared (EnsurePipelineDepth)
	int mCurrentPipelineTexture = 0;
	bool mSceneFBCurrent = true;

	static bool FailedCreate;

	friend class GLPPRenderState;
};

}
