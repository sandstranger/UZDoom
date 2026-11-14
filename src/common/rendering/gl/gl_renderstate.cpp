/*
** gl_renderstate.cpp
**
** Render state maintenance
**
**---------------------------------------------------------------------------
**
** Copyright 2009-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/


#include "gl_system.h"
#include "gl_interface.h"
#include "hw_cvars.h"
#include "flatvertices.h"
#include "gl_shader.h"
#include "gl_renderer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_renderbuffers.h"
#include "gl_hwtexture.h"
#include "gl_buffers.h"
#include "hw_clock.h"
#include "hw_batching.h"
#include "flatvertices.h"
#include "printf.h"
#include <stddef.h>
#include <algorithm>
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "texturemanager.h"

namespace OpenGLRenderer
{

FGLRenderState gl_RenderState;

static VSMatrix identityMatrix(1);

static void matrixToGL(const VSMatrix &mat, int loc)
{
	glUniformMatrix4fv(loc, 1, false, (float*)&mat);
}

//==========================================================================
//
// This only gets called once upon setup.
// With OpenGL the state is persistent and cannot be cleared, once set up.
//
//==========================================================================

void FGLRenderState::Reset()
{
	FRenderState::Reset();
	mVertexBuffer = mCurrentVertexBuffer = nullptr;
	mGlossiness = 0.0f;
	mSpecularLevel = 0.0f;
	mShaderTimer = 0.0f;

	stRenderStyle = DefaultRenderStyle();
	stSrcBlend = stDstBlend = -1;
	stBlendEquation = -1;
	stAlphaTest = 0;
	mLastDepthClamp = true;

	mEffectState = 0;
	activeShader = nullptr;

	mCurrentVertexBuffer = nullptr;
	mCurrentVertexOffsets[0] = mVertexOffsets[0] = 0;
	mCurrentIndexBuffer = nullptr;

	mDepthFunc = DF_Less;
	mCullMode = Cull_None;
	mDepthFuncDirty = mCullDirty = true;
	mInstancedShader = nullptr;
	// Called for a new context: whatever the old one owned is gone with it.
	mInstanceStream = {};
	mIndexStream = {};
	mBatchCaps = -1;
}

//==========================================================================
//
// Apply shader settings
//
//==========================================================================

bool FGLRenderState::ApplyShader()
{
	static const float nulvec[] = { 0.f, 0.f, 0.f, 0.f };
	FShader *previous = activeShader;
	if (mInstancedShader)
	{
		activeShader = mInstancedShader;
		activeShader->Bind();
	}
	else if (mSpecialEffect > EFF_NONE)
	{
		activeShader = GLRenderer->mShaderManager->BindEffect(mSpecialEffect, mPassType);
		if (activeShader && ClipEmulationActive())
		{
			activeShader = activeShader->GetClipVariant();
			activeShader->Bind();
		}
	}
	else
	{
		activeShader = GLRenderer->mShaderManager->Get(mTextureEnabled ? mEffectState : SHADER_NoTexture, mAlphaThreshold >= 0.f, mPassType);
		if (ClipEmulationActive()) activeShader = activeShader->GetClipVariant();
		activeShader->Bind();
	}
	if (activeShader != previous) batchStats.shaderSwitches++;

	int fogset = 0;

	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((GetFogColor() & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	glVertexAttrib4fv(VATTR_COLOR, &mStreamData.uVertexColor.X);
	glVertexAttrib4fv(VATTR_NORMAL, &mStreamData.uVertexNormal.X);

	activeShader->muDesaturation.Set(mStreamData.uDesaturationFactor);
	activeShader->muFogEnabled.Set(fogset);

	activeShader->muTextureMode.Set(GetTextureModeAndFlags(mTempTM));
	activeShader->muLightParms.Set(mLightParms);
	activeShader->muFogColor.Set(mStreamData.uFogColor);
	activeShader->muObjectColor.Set(mStreamData.uObjectColor);
	activeShader->muDynLightColor.Set(&mStreamData.uDynLightColor.X);
	activeShader->muInterpolationFactor.Set(mStreamData.uInterpolationFactor);
	activeShader->muTimer.Set((double)(screen->FrameTime - firstFrame) * (double)mShaderTimer / 1000.);
	activeShader->muAlphaThreshold.Set(mAlphaThreshold);
	activeShader->muLightIndex.Set(-1);
	activeShader->muBoneIndexBase.Set(-1);
	activeShader->muClipSplit.Set(mClipSplit);
	activeShader->muSpecularMaterial.Set(mGlossiness, mSpecularLevel);
	activeShader->muAddColor.Set(mStreamData.uAddColor);
	activeShader->muTextureAddColor.Set(mStreamData.uTextureAddColor);
	activeShader->muTextureModulateColor.Set(mStreamData.uTextureModulateColor);
	activeShader->muTextureBlendColor.Set(mStreamData.uTextureBlendColor);
	activeShader->muDetailParms.Set(&mStreamData.uDetailParms.X);
#ifdef NPOT_EMULATION
	activeShader->muNpotEmulation.Set(&mStreamData.uNpotEmulation.X);
#endif

	if (mGlowEnabled || activeShader->currentglowstate)
	{
		activeShader->muGlowTopColor.Set(&mStreamData.uGlowTopColor.X);
		activeShader->muGlowBottomColor.Set(&mStreamData.uGlowBottomColor.X);
		activeShader->muGlowTopPlane.Set(&mStreamData.uGlowTopPlane.X);
		activeShader->muGlowBottomPlane.Set(&mStreamData.uGlowBottomPlane.X);
		activeShader->currentglowstate = mGlowEnabled;
	}

	if (mGradientEnabled || activeShader->currentgradientstate)
	{
		activeShader->muObjectColor2.Set(mStreamData.uObjectColor2);
		activeShader->muGradientTopPlane.Set(&mStreamData.uGradientTopPlane.X);
		activeShader->muGradientBottomPlane.Set(&mStreamData.uGradientBottomPlane.X);
		activeShader->currentgradientstate = mGradientEnabled;
	}

	if (mSplitEnabled || activeShader->currentsplitstate)
	{
		activeShader->muSplitTopPlane.Set(&mStreamData.uSplitTopPlane.X);
		activeShader->muSplitBottomPlane.Set(&mStreamData.uSplitBottomPlane.X);
		activeShader->currentsplitstate = mSplitEnabled;
	}


	if (mTextureMatrixEnabled)
	{
		matrixToGL(mTextureMatrix, activeShader->texturematrix_index);
		activeShader->currentTextureMatrixState = true;
	}
	else if (activeShader->currentTextureMatrixState)
	{
		activeShader->currentTextureMatrixState = false;
		matrixToGL(identityMatrix, activeShader->texturematrix_index);
	}

	if (mModelMatrixEnabled)
	{
		matrixToGL(mModelMatrix, activeShader->modelmatrix_index);
		VSMatrix norm;
		norm.computeNormalMatrix(mModelMatrix);
		matrixToGL(norm, activeShader->normalmodelmatrix_index);
		activeShader->currentModelMatrixState = true;
	}
	else if (activeShader->currentModelMatrixState)
	{
		activeShader->currentModelMatrixState = false;
		matrixToGL(identityMatrix, activeShader->modelmatrix_index);
		matrixToGL(identityMatrix, activeShader->normalmodelmatrix_index);
	}

	int index = mLightIndex;
	// Mess alert for crappy AncientGL!
	if (!screen->mLights->GetBufferType() && index >= 0)
	{
		size_t start, size;
		index = screen->mLights->GetBinding(index, &start, &size);

		if (start != mLastMappedLightIndex || screen->mPipelineNbr > 1) // If multiple buffers always bind
		{
			mLastMappedLightIndex = start;
			static_cast<GLDataBuffer*>(screen->mLights->GetBuffer())->BindRange(nullptr, start, size);
		}
	}

	activeShader->muLightIndex.Set(index);

	index = mBoneIndexBase;
	if (!screen->mBones->GetBufferType() && index >= 0) // Uniform buffer fallback support
	{
		size_t start, size;
		index = screen->mBones->GetBinding(index, &start, &size);

		if (start != mLastMappedBoneIndexBase || screen->mPipelineNbr > 1) // If multiple buffers always bind
		{
			mLastMappedBoneIndexBase = start;
			static_cast<GLDataBuffer*>(screen->mBones->GetBuffer())->BindRange(nullptr, start, size);
		}
	}
	activeShader->muBoneIndexBase.Set(index);

	return true;
}


//==========================================================================
//
// Apply State
//
//==========================================================================

void FGLRenderState::ApplyState()
{
	if (mRenderStyle != stRenderStyle)
	{
		ApplyBlendMode();
		stRenderStyle = mRenderStyle;
	}

	if (mSplitEnabled != stSplitEnabled)
	{
		if (gl.flags & RFL_NO_CLIP_PLANES)
		{
			// no hardware clip planes (OpenGL ES): the split planes are applied in the shaders
		}
		else if (mSplitEnabled)
		{
			glEnable(GL_CLIP_DISTANCE3);
			glEnable(GL_CLIP_DISTANCE4);
		}
		else
		{
			glDisable(GL_CLIP_DISTANCE3);
			glDisable(GL_CLIP_DISTANCE4);
		}
		stSplitEnabled = mSplitEnabled;
	}

	if (mMaterial.mChanged)
	{
		ApplyMaterial();
		mMaterial.mChanged = false;
	}

	if (mBias.mChanged)
	{
		if (mBias.mFactor == 0 && mBias.mUnits == 0)
		{
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
		else
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
		}
		glPolygonOffset(mBias.mFactor, mBias.mUnits);
		mBias.mChanged = false;
	}
}

void FGLRenderState::ApplyBuffers()
{
	if (mVertexBuffer != mCurrentVertexBuffer || mVertexOffsets[0] != mCurrentVertexOffsets[0] || mVertexOffsets[1] != mCurrentVertexOffsets[1])
	{
		assert(mVertexBuffer != nullptr);
		static_cast<GLVertexBuffer*>(mVertexBuffer)->Bind(mVertexOffsets);
		batchStats.bufferBinds++;
		mCurrentVertexBuffer = mVertexBuffer;
		mCurrentVertexOffsets[0] = mVertexOffsets[0];
		mCurrentVertexOffsets[1] = mVertexOffsets[1];
	}
	if (mIndexBuffer != mCurrentIndexBuffer)
	{
		if (mIndexBuffer)
		{
			static_cast<GLIndexBuffer*>(mIndexBuffer)->Bind();
			batchStats.bufferBinds++;
		}
		mCurrentIndexBuffer = mIndexBuffer;
	}
}

void FGLRenderState::ApplyDepthAndCulling()
{
	if (mDepthFuncDirty)
	{
		static int df2gl[] = { GL_LESS, GL_LEQUAL, GL_ALWAYS };
		glDepthFunc(df2gl[mDepthFunc]);
		mDepthFuncDirty = false;
	}
	if (mCullDirty)
	{
		if (mCullMode != Cull_None)
		{
			glEnable(GL_CULL_FACE);
			glFrontFace(mCullMode == Cull_CCW ? GL_CCW : GL_CW);
		}
		else
		{
			glDisable(GL_CULL_FACE);
		}
		mCullDirty = false;
	}
}

void FGLRenderState::Apply()
{
	ApplyState();
	ApplyDepthAndCulling();
	ApplyBuffers();
	ApplyShader();
}

//===========================================================================
//
//	Binds a texture to the renderer
//
//===========================================================================

void FGLRenderState::ApplyMaterial()
{
	if (mMaterial.mMaterial->Source()->isHardwareCanvas() && !mMaterial.mMaterial->Source()->GetTranslucency())
	{
		mTempTM = TM_OPAQUE;
	}
	else
	{
		mTempTM = TM_NORMAL;
	}
	auto tex = mMaterial.mMaterial->Source();
	mEffectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : mMaterial.mMaterial->GetShaderIndex();
	mShaderTimer = tex->GetShaderSpeed();
	SetSpecular(tex->GetGlossiness(), tex->GetSpecularLevel());
	if (tex->isHardwareCanvas()) static_cast<FCanvasTexture*>(tex->GetTexture())->NeedUpdate();

	int clampmode = tex->GetClampMode(mMaterial.mClampMode);

	// avoid rebinding the same texture multiple times.
	if (mMaterial.mMaterial == lastMaterial && lastClamp == clampmode && mMaterial.mTranslation == lastTranslation) return;
	lastMaterial = mMaterial.mMaterial;
	lastClamp = clampmode;
	lastTranslation = mMaterial.mTranslation;
	batchStats.textureSwitches++;

	int maxbound = 0;

	int numLayers = mMaterial.mMaterial->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mMaterial.mMaterial->GetLayer(0, mMaterial.mTranslation, &layer));

	if (base->BindOrCreate(tex->GetTexture(), 0, clampmode, mMaterial.mTranslation, layer->scaleFlags))
	{
		if (!(layer->scaleFlags & CTF_Indexed))
		{
			for (int i = 1; i < numLayers; i++)
			{
				auto systex = static_cast<FHardwareTexture*>(mMaterial.mMaterial->GetLayer(i, 0, &layer));
				// fixme: Upscale flags must be disabled for certain layers.
				systex->BindOrCreate(layer->layerTexture, i, clampmode, 0, layer->scaleFlags);
				maxbound = i;
			}
		}
		else
		{
			for (int i = 1; i < 3; i++)
			{
				auto systex = static_cast<FHardwareTexture*>(mMaterial.mMaterial->GetLayer(i, mMaterial.mTranslation, &layer));
				systex->Bind(i, false);
				maxbound = i;
			}
		}
	}
	// unbind everything from the last texture that's still active
	for (int i = maxbound + 1; i <= maxBoundMaterial; i++)
	{
		FHardwareTexture::Unbind(i);
		maxBoundMaterial = maxbound;
	}
}

//==========================================================================
//
// Apply blend mode from RenderStyle
//
//==========================================================================

void FGLRenderState::ApplyBlendMode()
{
	static int blendstyles[] = { GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
	static int renderops[] = { 0, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1 };

	int srcblend = blendstyles[mRenderStyle.SrcAlpha%STYLEALPHA_MAX];
	int dstblend = blendstyles[mRenderStyle.DestAlpha%STYLEALPHA_MAX];
	int blendequation = renderops[mRenderStyle.BlendOp & 15];

	if (blendequation == -1)	// This was a fuzz style.
	{
		srcblend = GL_DST_COLOR;
		dstblend = GL_ONE_MINUS_SRC_ALPHA;
		blendequation = GL_FUNC_ADD;
	}

	// Checks must be disabled until all draw code has been converted.
	//if (srcblend != stSrcBlend || dstblend != stDstBlend)
	{
		stSrcBlend = srcblend;
		stDstBlend = dstblend;
		glBlendFunc(srcblend, dstblend);
	}
	//if (blendequation != stBlendEquation)
	{
		stBlendEquation = blendequation;
		glBlendEquation(blendequation);
	}

}

//==========================================================================
//
// API dependent draw calls
//
//==========================================================================

static int dt2gl[] = { GL_POINTS, GL_LINES, GL_TRIANGLES, GL_TRIANGLE_FAN, GL_TRIANGLE_STRIP };

void FGLRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (mPendingBatches && !mFlushingBatch)
	{
		FlushBatches(BB_Draw);
		apply = true;	// the flush changed the GL state
	}
	batchStats.draws++;
	if (apply)
	{
		Apply();
	}
	drawcalls.Clock();
	glDrawArrays(dt2gl[dt], index, count);
	drawcalls.Unclock();
}

void FGLRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (mPendingBatches && !mFlushingBatch)
	{
		FlushBatches(BB_Draw);
		apply = true;
	}
	batchStats.draws++;
	if (apply)
	{
		Apply();
	}
	drawcalls.Clock();
	glDrawElements(dt2gl[dt], count, GL_UNSIGNED_INT, (void*)(intptr_t)(index * sizeof(uint32_t)));
	drawcalls.Unclock();
}

void FGLRenderState::SetDepthMask(bool on)
{
	FlushForImmediate();
	glDepthMask(on);
}

void FGLRenderState::SetDepthFunc(int func)
{
	// applied by the next draw (ApplyDepthAndCulling); part of the batch key meanwhile
	mDepthFunc = func;
	mDepthFuncDirty = true;
}

void FGLRenderState::SetDepthRange(float min, float max)
{
	FlushForImmediate();
	glDepthRange(min, max);
}

void FGLRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	FlushForImmediate();
	glColorMask(r, g, b, a);
}

void FGLRenderState::SetStencil(int offs, int op, int flags = -1)
{
	FlushForImmediate();
	static int op2gl[] = { GL_KEEP, GL_INCR, GL_DECR };

	glStencilFunc(GL_EQUAL, screen->stencilValue + offs, ~0);		// draw sky into stencil
	glStencilOp(GL_KEEP, GL_KEEP, op2gl[op]);		// this stage doesn't modify the stencil

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		glColorMask(cmon, cmon, cmon, cmon);						// don't write to the graphics buffer
		glDepthMask(!(flags & SF_DepthMaskOff));
	}
}

void FGLRenderState::ToggleState(int state, bool on)
{
	if (on)
	{
		glEnable(state);
	}
	else
	{
		glDisable(state);
	}
}

void FGLRenderState::SetCulling(int mode)
{
	// applied by the next draw (ApplyDepthAndCulling); part of the batch key meanwhile
	mCullMode = mode;
	mCullDirty = true;
}

void FGLRenderState::EnableClipDistance(int num, bool state)
{
	FlushForImmediate();
	// Update the viewpoint-related clip plane setting.
	if (num == 0) mClipPlane0 = state;
	if (!(gl.flags & RFL_NO_CLIP_PLANES))
	{
		ToggleState(GL_CLIP_DISTANCE0 + num, state);
	}
}

void FGLRenderState::Clear(int targets)
{
	FlushForImmediate();
	// This always clears to default values.
	int gltarget = 0;
	if (targets & CT_Depth)
	{
		gltarget |= GL_DEPTH_BUFFER_BIT;
		glClearDepth(1);
	}
	if (targets & CT_Stencil)
	{
		gltarget |= GL_STENCIL_BUFFER_BIT;
		glClearStencil(0);
	}
	if (targets & CT_Color)
	{
		gltarget |= GL_COLOR_BUFFER_BIT;
		glClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
	}
	glClear(gltarget);
}

void FGLRenderState::EnableStencil(bool on)
{
	FlushForImmediate();
	ToggleState(GL_STENCIL_TEST, on);
}

void FGLRenderState::SetScissor(int x, int y, int w, int h)
{
	FlushForImmediate();
	if (w > -1)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(x, y, w, h);
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);
	}
}

void FGLRenderState::SetViewport(int x, int y, int w, int h)
{
	FlushForImmediate();
	glViewport(x, y, w, h);
}

void FGLRenderState::EnableDepthTest(bool on)
{
	FlushForImmediate();
	ToggleState(GL_DEPTH_TEST, on);
}

void FGLRenderState::EnableMultisampling(bool on)
{
	FlushForImmediate();
	if (gl.esversion == 0) ToggleState(GL_MULTISAMPLE, on);	// always on for multisampled targets in OpenGL ES
}

void FGLRenderState::EnableLineSmooth(bool on)
{
	FlushForImmediate();
	if (gl.esversion == 0) ToggleState(GL_LINE_SMOOTH, on);	// not in OpenGL ES
}

//==========================================================================
//
//
//
//==========================================================================
void FGLRenderState::ClearScreen()
{
	FlushForImmediate();
	bool multi = gl.esversion == 0 && glIsEnabled(GL_MULTISAMPLE);

	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply();

	if (multi) glDisable(GL_MULTISAMPLE);
	glDisable(GL_DEPTH_TEST);

	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::FULLSCREEN_INDEX, 4);

	glEnable(GL_DEPTH_TEST);
	if (multi) glEnable(GL_MULTISAMPLE);
}



//==========================================================================
//
// Below are less frequently altrered state settings which do not get
// buffered by the state object, but set directly instead.
//
//==========================================================================

bool FGLRenderState::SetDepthClamp(bool on)
{
	FlushForImmediate();
	bool res = mLastDepthClamp;
	if (gl.esversion == 0)	// OpenGL ES 3.0 has no depth clamping
	{
		if (!on) glDisable(GL_DEPTH_CLAMP);
		else glEnable(GL_DEPTH_CLAMP);
	}
	mLastDepthClamp = on;
	return res;
}

//==========================================================================
//
// Draw batching / GPU instancing (hw_batching.cpp)
//
//==========================================================================

bool FGLRenderState::BatchingSupported(EBatchKind kind)
{
	if (mBatchCaps < 0)
	{
		// The entry points are looked up by name; that alone does not prove the context has
		// them, so the version of the context that was actually created decides.
		const char *version = (const char *)glGetString(GL_VERSION);
		int major = 0, minor = 0;
		bool es = version && strncmp(version, "OpenGL ES", 9) == 0;
		if (version) sscanf(es ? version + 9 : version, "%*[^0-9]%d.%d", &major, &minor);
		if (version && !es) sscanf(version, "%d.%d", &major, &minor);
		bool core = es ? major >= 3 : (major > 3 || (major == 3 && minor >= 3));

		GLint maxAttribs = 0;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
		bool instancing = core && glDrawArraysInstanced && glDrawElementsInstanced && glVertexAttribDivisor && glVertexAttribIPointer &&
			maxAttribs >= BATCH_MIN_VERTEX_ATTRIBS;
		// 32-bit indices are core in GLES 3.0 and desktop GL
		bool merge = core && glBufferSubData;
		mBatchCaps = (merge ? 1 : 0) | (instancing ? 2 : 0);
		DPrintf(DMSG_NOTIFY, "Draw batching: merged draws %s, instancing %s (GL_MAX_VERTEX_ATTRIBS %d)\n",
			merge ? "available" : "unavailable", instancing ? "available" : "unavailable", maxAttribs);
	}
	return (mBatchCaps & (kind == BK_Merge ? 1 : 2)) != 0;
}

// Streams data into a buffer that is only ever appended to; when it is full the storage is
// orphaned (glBufferData with no data), so no region the GPU may still read is overwritten
// and no CPU/GPU synchronisation is needed. Returns the byte offset or SIZE_MAX.
size_t FGLRenderState::UploadStream(StreamBuffer &buf, unsigned int target, size_t maxCapacity, const void *data, size_t bytes)
{
	if (bytes == 0 || bytes > maxCapacity) return SIZE_MAX;
	double t0 = BatchStatsTiming() ? BatchTimeMs() : 0;

	bool allocate = false;
	if (buf.id == 0)
	{
		glGenBuffers(1, &buf.id);
		buf.capacity = std::min(maxCapacity, std::max<size_t>(bytes * 4, 64 * 1024));
		allocate = true;
	}
	glBindBuffer(target, buf.id);
	if (bytes > buf.capacity)
	{
		buf.capacity = std::min(maxCapacity, std::max(bytes, buf.capacity * 2));
		allocate = true;
	}
	else if (buf.pos + bytes > buf.capacity)
	{
		allocate = true;	// orphan
	}
	if (allocate)
	{
		while (glGetError() != GL_NO_ERROR) {}
		glBufferData(target, buf.capacity, nullptr, GL_STREAM_DRAW);
		buf.pos = 0;
		if (glGetError() == GL_OUT_OF_MEMORY)
		{
			// Degrade to unbatched drawing instead of failing.
			glBindBuffer(target, 0);
			glDeleteBuffers(1, &buf.id);
			buf = {};
			mBatchCaps = 0;
			Printf("Draw batching disabled: out of GPU memory for the stream buffer\n");
			return SIZE_MAX;
		}
	}
	size_t offset = buf.pos;
	glBufferSubData(target, offset, bytes, data);
	buf.pos = (offset + bytes + 15) & ~size_t(15);

	batchStats.instanceUploads++;
	batchStats.uploadBytes += bytes;
	if (t0 != 0) batchStats.uploadMs += BatchTimeMs() - t0;
	return offset;
}

bool FGLRenderState::DrawInstancedBatch(FDrawBatch &batch)
{
	const int variant = batch.kind == BK_Quad ? 1 : 2;
	unsigned count = batch.instances.Size();

	// The material decides the shader (ApplyMaterial sets mEffectState), so apply it first.
	ApplyState();
	if (mSpecialEffect > EFF_NONE) return false;
	FShader *shader = GLRenderer->mShaderManager->GetInstanced(mTextureEnabled ? mEffectState : SHADER_NoTexture, mAlphaThreshold >= 0.f, mPassType, variant, ClipEmulationActive());
	if (!shader) return false;

#if defined(__arm__) || defined(__i386__)
	const size_t maxStream = 512 * 1024;
#else
	const size_t maxStream = 2048 * 1024;
#endif
	size_t offset = UploadStream(mInstanceStream, GL_ARRAY_BUFFER, maxStream, batch.instances.Data(), count * sizeof(FInstanceGPU));
	if (offset == SIZE_MAX) return false;

	mInstancedShader = shader;
	ApplyDepthAndCulling();
	ApplyBuffers();
	ApplyShader();
	mInstancedShader = nullptr;

	// Per-instance attributes. The per-vertex ones (bound by ApplyBuffers) keep divisor 0.
	glBindBuffer(GL_ARRAY_BUFFER, mInstanceStream.id);
	const GLsizei stride = sizeof(FInstanceGPU);
	auto attrib = [&](GLuint loc, size_t field)
	{
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, stride, (void *)(intptr_t)(offset + field));
		glVertexAttribDivisor(loc, 1);
	};
	for (int i = 0; i < 4; i++) attrib(VATTR_INSTANCE0 + i, offsetof(FInstanceGPU, xform) + i * 4 * sizeof(float));
	attrib(VATTR_COLOR, offsetof(FInstanceGPU, color));
	attrib(VATTR_INSTANCE_LIGHT, offsetof(FInstanceGPU, lightAttr));
	attrib(VATTR_INSTANCE_DYNLIGHT, offsetof(FInstanceGPU, dynLight));
	glEnableVertexAttribArray(VATTR_INSTANCE_COLORS);
	glVertexAttribIPointer(VATTR_INSTANCE_COLORS, 4, GL_UNSIGNED_INT, stride, (void *)(intptr_t)(offset + offsetof(FInstanceGPU, objectColor)));
	glVertexAttribDivisor(VATTR_INSTANCE_COLORS, 1);

	static const int dt2gl[] = { GL_POINTS, GL_LINES, GL_TRIANGLES, GL_TRIANGLE_FAN, GL_TRIANGLE_STRIP };
	drawcalls.Clock();
	if (batch.kind == BK_Quad)
	{
		// The reserved quad of the flat vertex buffer supplies the corner index in aPosition.x.
		glDrawArraysInstanced(GL_TRIANGLE_STRIP, FFlatVertexBuffer::QUAD_INDEX, 4, count);
	}
	else if (batch.key.indexed)
	{
		glDrawElementsInstanced(dt2gl[batch.key.drawType], batch.key.count, GL_UNSIGNED_INT, (void *)(intptr_t)(batch.key.first * sizeof(uint32_t)), count);
	}
	else
	{
		glDrawArraysInstanced(dt2gl[batch.key.drawType], batch.key.first, batch.key.count, count);
	}
	drawcalls.Unclock();
	batchStats.instancedDraws++;

	// Leave no divisor or enabled instance array behind for the regular draws.
	static const GLuint locs[] = { VATTR_INSTANCE0, VATTR_INSTANCE0 + 1, VATTR_INSTANCE0 + 2, VATTR_INSTANCE0 + 3, VATTR_COLOR,
		VATTR_INSTANCE_LIGHT, VATTR_INSTANCE_DYNLIGHT, VATTR_INSTANCE_COLORS };
	for (GLuint loc : locs)
	{
		glVertexAttribDivisor(loc, 0);
		glDisableVertexAttribArray(loc);
	}
	mCurrentVertexBuffer = nullptr;	// re-establish the per-vertex layout with the next draw
	return true;
}

bool FGLRenderState::DrawMergedBatch(FDrawBatch &batch)
{
	auto vd = screen->mVertexData;
	mMergedIndices.Resize(batch.mergedIndices);
	BuildMergedIndices(batch, vd->ibo_data.Data(), vd->ibo_data.Size(), mMergedIndices.Data());

	Apply();
#if defined(__arm__) || defined(__i386__)
	const size_t maxStream = 1024 * 1024;
#else
	const size_t maxStream = 4096 * 1024;
#endif
	size_t offset = UploadStream(mIndexStream, GL_ELEMENT_ARRAY_BUFFER, maxStream, mMergedIndices.Data(), batch.mergedIndices * sizeof(uint32_t));
	if (offset == SIZE_MAX)
	{
		mCurrentIndexBuffer = nullptr;
		return false;
	}
	drawcalls.Clock();
	glDrawElements(GL_TRIANGLES, batch.mergedIndices, GL_UNSIGNED_INT, (void *)(intptr_t)offset);
	drawcalls.Unclock();
	batchStats.mergedDraws++;
	mCurrentIndexBuffer = nullptr;	// the stream buffer is bound as element array buffer now
	return true;
}

void FGLRenderState::ReleaseBatchResources()
{
	if (mInstanceStream.id) glDeleteBuffers(1, &mInstanceStream.id);
	if (mIndexStream.id) glDeleteBuffers(1, &mIndexStream.id);
	mInstanceStream = {};
	mIndexStream = {};
	mMergedIndices.Reset();
	mBatchCaps = -1;
}

}
