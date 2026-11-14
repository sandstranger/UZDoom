/*
** gles_renderstate.cpp
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


#include "gles_system.h"
#include "hw_cvars.h"
#include "flatvertices.h"
#include "gles_shader.h"
#include "gles_renderer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gles_renderbuffers.h"
#include "gles_hwtexture.h"
#include "gles_buffers.h"
#include "gles_renderer.h"
#include "gles_samplers.h"

#include "hw_clock.h"
#include "printf.h"

#include "hwrenderer/data/hw_viewpointbuffer.h"

#include "hw_batching.h"
#include "engineerrors.h"
#include "flatvertices.h"
#include "printf.h"
#include <stddef.h>
#include <algorithm>

namespace OpenGLESRenderer
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
	mInstancedVariant = 0;
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

	ShaderFlavourData flavour;

	// Need to calc light data now in order to select correct shader
	float* lightPtr = NULL;
	int modLights = 0;
	int subLights = 0;
	int addLights = 0;
	int totalLights = 0;

	flavour.hasSpotLight = false;

	if (mLightIndex >= 0)
	{
		lightPtr = ((float*)screen->mLights->GetBuffer()->Memory());
		lightPtr += ((int64_t)mLightIndex * 4);

		// Calculate how much light data there is to upload, this is stored in the first 4 floats
		modLights = int(lightPtr[1]) / LIGHT_VEC4_NUM;
		subLights = (int(lightPtr[2]) - int(lightPtr[1])) / LIGHT_VEC4_NUM;
		addLights = (int(lightPtr[3]) - int(lightPtr[2])) / LIGHT_VEC4_NUM;

		// Here we limit the number of lights, but don't change the light data so priority has to be mod, sub then add
		if (modLights > (int)gles.maxlights)
			modLights = gles.maxlights;

		if (modLights + subLights > (int)gles.maxlights)
			subLights = gles.maxlights - modLights;

		if (modLights + subLights + addLights > (int)gles.maxlights)
			addLights = gles.maxlights - modLights - subLights;

		totalLights = modLights + subLights + addLights;

		// Skip passed the first 4 floats so the upload below only contains light data
		lightPtr += 4;

		float* findSpotsPtr = lightPtr + 11; // The 11th float contains '1' if the light is a spot light, see hw_dynlightdata.cpp

		for (int n = 0; n < totalLights; n++)
		{
			if (*findSpotsPtr > 0) // This is a spot light
			{
				flavour.hasSpotLight = true;
				break;
			}
			findSpotsPtr += LIGHT_VEC4_NUM * 4;
		}
	}


	int tm = GetTextureModeAndFlags(mTempTM);
	flavour.textureMode = tm & 0xffff;
	flavour.texFlags = tm >> 16; //Move flags to start of word

	if (mTextureClamp && flavour.textureMode == TM_NORMAL) flavour.textureMode = TM_CLAMPY; // fixme. Clamp can now be combined with all modes.

	if (flavour.textureMode == -1)
		flavour.textureMode = 0;


	flavour.blendFlags = (int)(mStreamData.uTextureAddColor.a + 0.01);
	flavour.paletteInterpolate = !!(flavour.blendFlags & 0x4000);

	flavour.twoDFog = false;
	flavour.fogEnabled = false;
	flavour.fogEquationRadial = false;
	flavour.colouredFog = false;

	flavour.fogEquationRadial = (gl_fogmode == 2);

	flavour.twoDFog = false;
	flavour.fogEnabled = false;
	flavour.colouredFog = false;

	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			flavour.twoDFog = true;
		}
		else if (gl_fogmode)
		{
			flavour.fogEnabled = true;

			if ((GetFogColor() & 0xffffff) != 0)
				flavour.colouredFog = true;
		}
	}

	flavour.doDesaturate = mStreamData.uDesaturationFactor != 0;
	flavour.useULightLevel = (mLightParms[3] >= 0); //#define uLightLevel uLightAttr.a

	// Yes create shaders for all combinations of active lights to avoid more branches
	flavour.dynLightsMod = (modLights > 0);
	flavour.dynLightsSub = (subLights > 0);
	flavour.dynLightsAdd = (addLights > 0);

	flavour.useObjectColor2 = (mStreamData.uObjectColor2.a > 0);
	flavour.useGlowTopColor = mGlowEnabled && (mStreamData.uGlowTopColor[3] > 0);
	flavour.useGlowBottomColor = mGlowEnabled && (mStreamData.uGlowBottomColor[3] > 0);

	flavour.useColorMap = (mColorMapSpecial >= CM_FIRSTSPECIALCOLORMAP) || (mColorMapFlash != 1);

	flavour.buildLighting = (mHwUniforms->mPalLightLevels >> 16) == 5; // Build engine mode
	flavour.bandedSwLight = !!(mHwUniforms->mPalLightLevels & 0xFF);

#ifdef NPOT_EMULATION
	flavour.npotEmulation = (mStreamData.uNpotEmulation.Y != 0);
#endif

	flavour.instancing = mInstancedVariant;

	auto previous = activeShader ? activeShader->cur : nullptr;
	if (mSpecialEffect > EFF_NONE)
	{
		activeShader = GLRenderer->mShaderManager->BindEffect(mSpecialEffect, mPassType, flavour);
	}
	else
	{
		activeShader = GLRenderer->mShaderManager->Get(mTextureEnabled ? mEffectState : SHADER_NoTexture, mAlphaThreshold >= 0.f, mPassType);

		activeShader->Bind(flavour);
	}
	if (activeShader && activeShader->cur != previous) batchStats.shaderSwitches++;


	if (mHwUniforms)
	{
		activeShader->cur->muProjectionMatrix.Set(&mHwUniforms->ProjectionMatrix);
		activeShader->cur->muViewMatrix.Set(&mHwUniforms->ViewMatrix);
		activeShader->cur->muNormalViewMatrix.Set(&mHwUniforms->NormalViewMatrix);
		activeShader->cur->muCameraPos.Set(&mHwUniforms->mCameraPos.X);
		activeShader->cur->muClipLine.Set(&mHwUniforms->mClipLine.X);
		activeShader->cur->muGlobVis.Set(mHwUniforms->mGlobVis);
		activeShader->cur->muPalLightLevels.Set(mHwUniforms->mPalLightLevels & 0xFF); // JUST pass the pal levels, clear the top bits
		activeShader->cur->muViewHeight.Set(mHwUniforms->mViewHeight);
		activeShader->cur->muClipHeight.Set(mHwUniforms->mClipHeight);
		activeShader->cur->muClipHeightDirection.Set(mHwUniforms->mClipHeightDirection);
		//activeShader->cur->muShadowmapFilter.Set(mHwUniforms->mShadowmapFilter);
		activeShader->cur->muThickFogDistance.Set(mHwUniforms->mThickFogDistance);
		activeShader->cur->muThickFogMultiplier.Set(mHwUniforms->mThickFogMultiplier);
	}

	glVertexAttrib4fv(VATTR_COLOR, &mStreamData.uVertexColor.X);
	glVertexAttrib4fv(VATTR_NORMAL, &mStreamData.uVertexNormal.X);

	activeShader->cur->muDesaturation.Set(mStreamData.uDesaturationFactor);
	//activeShader->cur->muFogEnabled.Set(fogset);

	activeShader->cur->muLightParms.Set(mLightParms);
	activeShader->cur->muFogColor.Set(mStreamData.uFogColor);
	activeShader->cur->muObjectColor.Set(mStreamData.uObjectColor);
	activeShader->cur->muDynLightColor.Set(&mStreamData.uDynLightColor.X);
	activeShader->cur->muInterpolationFactor.Set(mStreamData.uInterpolationFactor);
	activeShader->cur->muTimer.Set((double)(screen->FrameTime - firstFrame) * (double)mShaderTimer / 1000.);
	activeShader->cur->muAlphaThreshold.Set(mAlphaThreshold);
	activeShader->cur->muBoneIndexBase.Set(-1);
	activeShader->cur->muClipSplit.Set(mClipSplit);
	activeShader->cur->muSpecularMaterial.Set(mGlossiness, mSpecularLevel);
	activeShader->cur->muAddColor.Set(mStreamData.uAddColor);
	activeShader->cur->muTextureAddColor.Set(mStreamData.uTextureAddColor);
	activeShader->cur->muTextureModulateColor.Set(mStreamData.uTextureModulateColor);
	activeShader->cur->muTextureBlendColor.Set(mStreamData.uTextureBlendColor);
	activeShader->cur->muDetailParms.Set(&mStreamData.uDetailParms.X);
#ifdef NPOT_EMULATION
	activeShader->cur->muNpotEmulation.Set(&mStreamData.uNpotEmulation.X);
#endif

	if (flavour.useColorMap)
	{
		if (mColorMapSpecial < CM_FIRSTSPECIALCOLORMAP || mColorMapSpecial >= CM_MAXCOLORMAP)
		{
			activeShader->cur->muFixedColormapStart.Set( 0,0,0, mColorMapFlash );
			activeShader->cur->muFixedColormapRange.Set( 0,0,0, 1.f );
		}
		else
		{
			FSpecialColormap* scm = &SpecialColormaps[mColorMapSpecial - CM_FIRSTSPECIALCOLORMAP];

			//uniforms.MapStart = { scm->ColorizeStart[0], scm->ColorizeStart[1], scm->ColorizeStart[2], flash };
			activeShader->cur->muFixedColormapStart.Set( scm->ColorizeStart[0], scm->ColorizeStart[1], scm->ColorizeStart[2], mColorMapFlash );
			activeShader->cur->muFixedColormapRange.Set( scm->ColorizeEnd[0] - scm->ColorizeStart[0],
				scm->ColorizeEnd[1] - scm->ColorizeStart[1], scm->ColorizeEnd[2] - scm->ColorizeStart[2], 0.f );
		}
	}

	if (mGlowEnabled || activeShader->cur->currentglowstate)
	{
		activeShader->cur->muGlowTopColor.Set(&mStreamData.uGlowTopColor.X);
		activeShader->cur->muGlowBottomColor.Set(&mStreamData.uGlowBottomColor.X);
		activeShader->cur->muGlowTopPlane.Set(&mStreamData.uGlowTopPlane.X);
		activeShader->cur->muGlowBottomPlane.Set(&mStreamData.uGlowBottomPlane.X);
		activeShader->cur->currentglowstate = mGlowEnabled;
	}

	if (mGradientEnabled || activeShader->cur->currentgradientstate)
	{
		activeShader->cur->muObjectColor2.Set(mStreamData.uObjectColor2);
		activeShader->cur->muGradientTopPlane.Set(&mStreamData.uGradientTopPlane.X);
		activeShader->cur->muGradientBottomPlane.Set(&mStreamData.uGradientBottomPlane.X);
		activeShader->cur->currentgradientstate = mGradientEnabled;
	}

	if (mSplitEnabled || activeShader->cur->currentsplitstate)
	{
		activeShader->cur->muSplitTopPlane.Set(&mStreamData.uSplitTopPlane.X);
		activeShader->cur->muSplitBottomPlane.Set(&mStreamData.uSplitBottomPlane.X);
		activeShader->cur->currentsplitstate = mSplitEnabled;
	}


	if (mTextureMatrixEnabled)
	{
		matrixToGL(mTextureMatrix, activeShader->cur->texturematrix_index);
		activeShader->cur->currentTextureMatrixState = true;
	}
	else if (activeShader->cur->currentTextureMatrixState)
	{
		activeShader->cur->currentTextureMatrixState = false;
		matrixToGL(identityMatrix, activeShader->cur->texturematrix_index);
	}

	if (mModelMatrixEnabled)
	{
		matrixToGL(mModelMatrix, activeShader->cur->modelmatrix_index);
		VSMatrix norm;
		norm.computeNormalMatrix(mModelMatrix);
		matrixToGL(norm, activeShader->cur->normalmodelmatrix_index);
		activeShader->cur->currentModelMatrixState = true;
	}
	else if (activeShader->cur->currentModelMatrixState)
	{
		activeShader->cur->currentModelMatrixState = false;
		matrixToGL(identityMatrix, activeShader->cur->modelmatrix_index);
		matrixToGL(identityMatrix, activeShader->cur->normalmodelmatrix_index);
	}

	// Upload the light data
	if (mLightIndex >= 0)
	{
		// Calculate the total number of vec4s we need
		int totalVectors = totalLights * LIGHT_VEC4_NUM;

		if (totalVectors > (int)gles.numlightvectors)
			totalVectors = gles.numlightvectors;

		glUniform4fv(activeShader->cur->lights_index, totalVectors, lightPtr);

		int range[4] = { 0,
			modLights * LIGHT_VEC4_NUM,
			(modLights + subLights) * LIGHT_VEC4_NUM,
			(modLights + subLights + addLights) * LIGHT_VEC4_NUM };

		activeShader->cur->muLightRange.Set(range);
	}

	if (gles.glesMode >= GLES_MODE_OGL3)
	{
		// Upload bone data
		// NOTE, this is pretty inefficient, it will be reloading the same data over and over in a single frame
		// Need to add something to detect a start of new frame then only update the data when it's been changed
		if ((mBoneIndexBase >= 0))
		{
			float* bonesPtr = ((float*)screen->mBones->GetBuffer()->Memory());

			int number = screen->mBones->GetCurrentIndex();

			glUniformMatrix4fv(activeShader->cur->bones_index, number, false, bonesPtr);

			activeShader->cur->muBoneIndexBase.Set(mBoneIndexBase);
		}
	}

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
		stSplitEnabled = mSplitEnabled;
	}

	if (mMaterial.mChanged)
	{
		ApplyMaterial(mMaterial.mMaterial, mMaterial.mClampMode, mMaterial.mTranslation, mMaterial.mOverrideShader);
		mMaterial.mChanged = false;
	}


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

void FGLRenderState::ApplyMaterial(FMaterial *mat, int clampmode, int translation, int overrideshader)
{
	if (mat->Source()->isHardwareCanvas() && !mat->Source()->GetTranslucency())
	{
		mTempTM = TM_OPAQUE;
	}
	else
	{
		mTempTM = TM_NORMAL;
	}
	auto tex = mat->Source();
	mEffectState = overrideshader >= 0 ? overrideshader : mat->GetShaderIndex();
	mShaderTimer = tex->GetShaderSpeed();
	SetSpecular(tex->GetGlossiness(), tex->GetSpecularLevel());
	if (tex->isHardwareCanvas()) static_cast<FCanvasTexture*>(tex->GetTexture())->NeedUpdate();

	clampmode = tex->GetClampMode(clampmode);

	// avoid rebinding the same texture multiple times.
	if (mat == lastMaterial && lastClamp == clampmode && translation == lastTranslation) return;
	lastMaterial = mat;
	lastClamp = clampmode;
	lastTranslation = translation;

	int maxbound = 0;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(tex->GetTexture(), 0, clampmode, translation, layer->scaleFlags))
	{
		if (!(layer->scaleFlags & CTF_Indexed))
		{
			for (int i = 1; i < numLayers; i++)
			{
				auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
				// fixme: Upscale flags must be disabled for certain layers.
				systex->BindOrCreate(layer->layerTexture, i, clampmode, 0, layer->scaleFlags);
				maxbound = i;
			}
		}
		else
		{
			for (int i = 1; i < 3; i++)
			{
				auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, translation, &layer));
				GLRenderer->mSamplerManager->Bind(i, CLAMP_NONE, 255);
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
	if (srcblend != stSrcBlend || dstblend != stDstBlend)
	{
		stSrcBlend = srcblend;
		stDstBlend = dstblend;
		glBlendFunc(srcblend, dstblend);
	}
	if (blendequation != stBlendEquation)
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
	glDepthRangef(min, max);
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

}

void FGLRenderState::Clear(int targets)
{
	FlushForImmediate();
	// This always clears to default values.
	int gltarget = 0;
	if (targets & CT_Depth)
	{
		gltarget |= GL_DEPTH_BUFFER_BIT;

		glClearDepthf(1);
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

}

void FGLRenderState::EnableLineSmooth(bool on)
{

}


//==========================================================================
//
//
//
//==========================================================================
void FGLRenderState::ClearScreen()
{
	FlushForImmediate();

	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply();

	glDisable(GL_DEPTH_TEST);

	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::FULLSCREEN_INDEX, 4);

	glEnable(GL_DEPTH_TEST);

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

	if (gles.depthClampAvailable)
	{
		if (!on) glDisable(GL_DEPTH_CLAMP);
		else glEnable(GL_DEPTH_CLAMP);
	}

	mLastDepthClamp = on;
	return res;
}

//==========================================================================
//
// Draw batching / GPU instancing (hw_batching.cpp). Only in GLES3 mode: the instanced
// shader flavours need GLSL ES 3.00 and the draws need GLES 3.0 entry points.
//
//==========================================================================

bool FGLRenderState::BatchingSupported(EBatchKind kind)
{
	if (mBatchCaps < 0)
	{
		mBatchCaps = 0;
		const char *version = (const char *)glGetString(GL_VERSION);
		int major = 0;
		bool es = version && strncmp(version, "OpenGL ES", 9) == 0;
		if (version) sscanf(es ? version + 9 : version, "%*[^0-9]%d", &major);
		if (version && !es) sscanf(version, "%d", &major);
		// The entry points are looked up by name; the context version decides whether they exist.
		bool core = es && major >= 3 && gles.glesMode == GLES_MODE_GLES3;
		if (core && gles.essl300)
		{
			GLint maxAttribs = 0;
			glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
			bool instancing = glDrawArraysInstanced && glDrawElementsInstanced && glVertexAttribDivisor && glVertexAttribIPointer &&
				maxAttribs >= BATCH_MIN_VERTEX_ATTRIBS;
			mBatchCaps = 1 | (instancing ? 2 : 0);
			DPrintf(DMSG_NOTIFY, "Draw batching: merged draws available, instancing %s (GL_MAX_VERTEX_ATTRIBS %d)\n",
				instancing ? "available" : "unavailable", maxAttribs);
		}
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
	unsigned count = batch.instances.Size();
	if (mSpecialEffect > EFF_NONE) return false;

#if defined(__arm__) || defined(__i386__)
	const size_t maxStream = 512 * 1024;
#else
	const size_t maxStream = 2048 * 1024;
#endif
	size_t offset = UploadStream(mInstanceStream, GL_ARRAY_BUFFER, maxStream, batch.instances.Data(), count * sizeof(FInstanceGPU));
	if (offset == SIZE_MAX) return false;

	// The instanced flavour of the shader the regular draw would use. It is compiled on first
	// use; if the driver rejects it, instancing is switched off and the batch is replayed.
	mInstancedVariant = batch.kind == BK_Quad ? 1 : 2;
	try
	{
		Apply();
	}
	catch (CRecoverableError &err)
	{
		mInstancedVariant = 0;
		mBatchCaps &= ~2;
		Printf("Instanced shader flavour unavailable, drawing without instancing:\n%s\n", err.GetMessage());
		return false;
	}
	mInstancedVariant = 0;
	if (!activeShader || !activeShader->cur || activeShader->cur->hShader == 0) return false;

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

void FGLRenderState::ApplyViewport(void* data)
{
	// The viewpoint uniforms are read from this pointer by every draw, pending batches included.
	FlushForImmediate();
	mHwUniforms = reinterpret_cast<HWViewpointUniforms*>(static_cast<uint8_t*>(data));

}
}
