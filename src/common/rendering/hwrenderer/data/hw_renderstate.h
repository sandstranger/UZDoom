/*
** hw_renderstate.h
**
**
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
*/

#pragma once

#include "vectors.h"
#include "matrix.h"
#include "hw_material.h"
#include "texmanip.h"
#include "version.h"
#include "i_interface.h"

#include <string.h>
#include "tarray.h"

struct FColormap;
class IVertexBuffer;
class IIndexBuffer;
class PClass;

enum EClearTarget
{
	CT_Depth = 1,
	CT_Stencil = 2,
	CT_Color = 4
};

enum ERenderEffect
{
	EFF_NONE = -1,
	EFF_FOGBOUNDARY,
	EFF_SPHEREMAP,
	EFF_BURN,
	EFF_STENCIL,
	EFF_DITHERTRANS,
	MAX_EFFECTS
};

enum EAlphaFunc
{
	Alpha_GEqual = 0,
	Alpha_Greater = 1
};

enum EDrawType
{
	DT_Points = 0,
	DT_Lines = 1,
	DT_Triangles = 2,
	DT_TriangleFan = 3,
	DT_TriangleStrip = 4
};

enum EDepthFunc
{
	DF_Less,
	DF_LEqual,
	DF_Always
};

enum EStencilFlags
{
	SF_AllOn = 0,
	SF_ColorMaskOff = 1,
	SF_DepthMaskOff = 2,
};

enum EStencilOp
{
	SOP_Keep = 0,
	SOP_Increment = 1,
	SOP_Decrement = 2
};

enum ECull
{
	Cull_None,
	Cull_CCW,
	Cull_CW
};



struct FStateVec4
{
	float vec[4];

	void Set(float r, float g, float b, float a)
	{
		vec[0] = r;
		vec[1] = g;
		vec[2] = b;
		vec[3] = a;
	}
};

struct FMaterialState
{
	FMaterial *mMaterial = nullptr;
	int mClampMode;
	int mTranslation;
	int mOverrideShader;
	bool mChanged;

	void Reset()
	{
		mMaterial = nullptr;
		mTranslation = 0;
		mClampMode = CLAMP_NONE;
		mOverrideShader = -1;
		mChanged = false;
	}
};

struct FDepthBiasState
{
	float mFactor;
	float mUnits;
	bool mChanged;

	void Reset()
	{
		mFactor = 0;
		mUnits = 0;
		mChanged = false;
	}
};

enum EPassType
{
	NORMAL_PASS,
	GBUFFER_PASS,
	MAX_PASS_TYPES
};

struct FVector4PalEntry
{
	float r, g, b, a;

	bool operator==(const FVector4PalEntry &other) const
	{
		return r == other.r && g == other.g && b == other.b && a == other.a;
	}

	bool operator!=(const FVector4PalEntry &other) const
	{
		return r != other.r || g != other.g || b != other.b || a != other.a;
	}

	FVector4PalEntry &operator=(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a * normScale;
		return *this;
	}

	FVector4PalEntry& SetIA(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a;
		return *this;
	}

	FVector4PalEntry& SetFlt(float v1, float v2, float v3, float v4)
	{
		r = v1;
		g = v2;
		b = v3;
		a = v4;
		return *this;
	}

};

struct StreamData
{
	FVector4PalEntry uObjectColor;
	FVector4PalEntry uObjectColor2;
	FVector4 uDynLightColor;
	FVector4PalEntry uAddColor;
	FVector4PalEntry uTextureAddColor;
	FVector4PalEntry uTextureModulateColor;
	FVector4PalEntry uTextureBlendColor;
	FVector4PalEntry uFogColor;
	float uDesaturationFactor;
	float uInterpolationFactor;
	float timer;
	int useVertexData;
	FVector4 uVertexColor;
	FVector4 uVertexNormal;

	FVector4 uGlowTopPlane;
	FVector4 uGlowTopColor;
	FVector4 uGlowBottomPlane;
	FVector4 uGlowBottomColor;

	FVector4 uGradientTopPlane;
	FVector4 uGradientBottomPlane;

	FVector4 uSplitTopPlane;
	FVector4 uSplitBottomPlane;

	FVector4 uDetailParms;
	FVector4 uNpotEmulation;
	FVector4 padding1, padding2, padding3;
};

//==========================================================================
//
// Draw batching / GPU instancing (see hw_batching.cpp)
//
// Scene code submits batchable geometry through DrawQuad (sprites, particles),
// DrawModel (model surfaces) and DrawMerged (walls, flats). Inside a batch scope the
// render state groups submissions whose complete render state is identical apart from
// the per-instance values (position/transform, vertex colour, light parameters, dynamic
// sprite light, object/add/fog colour, desaturation). A batch is drawn with one instanced
// draw (quads, models) or one indexed draw over the concatenated geometry (merged walls
// and flats). Batches below the threshold, and every backend without instancing support,
// replay the original individual draws with the original state, so the result is the
// same as without batching.
//
//==========================================================================

enum EBatchCategory
{
	BC_Sprite,
	BC_Particle,
	BC_Model,
	BC_Wall,
	BC_Flat,
	BC_Count
};

enum EBatchKind
{
	BK_None,
	BK_Quad,		// instanced: one shared 4-vertex strip, corners + uv per instance
	BK_Model,		// instanced: one shared mesh range, model matrix per instance
	BK_Merge,		// several fans/strips/indexed triangle ranges -> one indexed triangle list
};

enum EBatchScope
{
	BS_None,		// no batching: every submission is drawn immediately
	BS_Ordered,		// only consecutive submissions are merged (translucent geometry, sorted lists)
	BS_Unordered,	// any open batch may receive the submission (opaque, depth-tested geometry)
};

enum EBatchBreak
{
	BB_StateChange,		// next submission had a different render state
	BB_Draw,			// a non-batchable draw followed
	BB_Immediate,		// an immediate render state change (depth, stencil, viewport...)
	BB_Capacity,		// the batch reached r_instancing_maxbatch / its index budget
	BB_ScopeEnd,		// end of a draw list
	BB_Evicted,			// unordered scope: too many open batches
	BB_Count
};

// One instance as uploaded to the GPU. The layout is mirrored by the instance vertex
// attributes (locations VATTR_INSTANCE0..) of both GL backends; keep them in sync.
struct FInstanceGPU
{
	float xform[4][4];		// quads: corner i = xyz, w = ul/ur/vt/vb; models: model matrix columns
	float color[4];			// uVertexColor (fed to aColor)
	float lightAttr[4];		// uLightAttr
	float dynLight[4];		// uDynLightColor
	uint32_t objectColor;	// uObjectColor as RGBA8 (r in the low byte)
	uint32_t addColor;		// uAddColor
	uint32_t fogColor;		// uFogColor
	float desaturation;		// uDesaturationFactor
};
static_assert(sizeof(FInstanceGPU) == 128, "instance record layout is mirrored by the shaders");

struct FBatchItem
{
	int drawType;
	int index;
	int count;
	int8_t indexed;
	int8_t category;
};

// Everything Apply() reads from FRenderState, in a flat layout without implicit padding,
// so that two snapshots can be compared with memcmp.
struct FRenderStateSnapshot
{
	FMaterial *material;
	IVertexBuffer *vertexBuffer;
	IIndexBuffer *indexBuffer;
	void *pointerPad;			// keeps the pointer block a multiple of 8 bytes on 32-bit targets
	StreamData streamData;
	float modelMatrix[16];
	float textureMatrix[16];
	float lightParms[4];
	float clipSplit[2];
	float alphaThreshold;
	float colorMapFlash;
	float biasFactor;
	float biasUnits;
	int32_t lightIndex;
	int32_t boneIndexBase;
	int32_t specialEffect;
	int32_t textureMode;
	int32_t textureClamp;
	int32_t textureModeFlags;
	int32_t softLight;
	int32_t colorMapSpecial;
	int32_t passType;
	int32_t clampMode;
	int32_t translation;
	int32_t overrideShader;
	int32_t vertexOffsets[2];
	uint32_t fogColor;
	uint32_t renderStyle;
	uint32_t flags;				// fog mode + enable bits
	uint32_t backendState;		// lazily applied backend state (depth func, culling)
	uint32_t derived;			// key only: shader-path selectors derived from per-instance values
	uint32_t kind;				// key only
	int32_t drawType;			// key only (quads, models)
	int32_t first;				// key only (quads, models)
	int32_t count;				// key only (quads, models)
	int32_t indexed;			// key only (quads, models)
	int32_t pad[2];				// explicit tail padding (the struct is compared with memcmp)
};

struct FDrawBatch
{
	EBatchKind kind = BK_None;
	uint64_t serial = 0;
	FRenderStateSnapshot state;		// the full state of the first submission
	FRenderStateSnapshot key;		// state with per-instance values cleared
	TArray<FInstanceGPU> instances;	// quads, models
	TArray<FBatchItem> items;		// one per submission (replay and merged geometry)
	unsigned mergedIndices = 0;		// merged batches: index count after triangulation
};

class FRenderState
{
protected:
	uint8_t mFogEnabled;
	uint8_t mTextureEnabled:1;
	uint8_t mGlowEnabled : 1;
	uint8_t mGradientEnabled : 1;
	uint8_t mModelMatrixEnabled : 1;
	uint8_t mTextureMatrixEnabled : 1;
	uint8_t mSplitEnabled : 1;
	uint8_t mBrightmapEnabled : 1;

	int mLightIndex;
	int mBoneIndexBase;
	int mSpecialEffect;
	int mTextureMode;
	int mTextureClamp;
	int mTextureModeFlags;
	int mSoftLight;
	float mLightParms[4];

	float mAlphaThreshold;
	float mClipSplit[2];


	int mColorMapSpecial;
	float mColorMapFlash;

	StreamData mStreamData = {};
	PalEntry mFogColor;

	FRenderStyle mRenderStyle;

	FMaterialState mMaterial;
	FDepthBiasState mBias;

	IVertexBuffer *mVertexBuffer;
	int mVertexOffsets[2];	// one per binding point
	IIndexBuffer *mIndexBuffer;

	EPassType mPassType = NORMAL_PASS;

	// Draw batching (hw_batching.cpp)
	static constexpr int MAX_OPEN_BATCHES = 8;
	EBatchScope mBatchScope = BS_None;
	bool mFlushingBatch = false;
	int mPendingBatches = 0;
	uint64_t mBatchSerial = 0;
	FDrawBatch mBatches[MAX_OPEN_BATCHES];

	// Backend hooks. The defaults disable batching entirely, so a backend that does not
	// implement them (Vulkan, desktop GL unless enabled, GLES2) draws exactly as before.
	virtual bool BatchingSupported(EBatchKind kind) { return false; }
	// Draw the batch with the batch's state already restored. Returning false makes the
	// caller replay the individual submissions instead (e.g. a shader variant failed to build).
	virtual bool DrawInstancedBatch(FDrawBatch &batch) { return false; }
	virtual bool DrawMergedBatch(FDrawBatch &batch) { return false; }
	virtual uint32_t GetBackendState() const { return 0; }
	virtual void SetBackendState(uint32_t state) {}

	void FlushBatch(FDrawBatch &batch, EBatchBreak reason);
	void ApplyInstanceValues(const FInstanceGPU &inst);
	FDrawBatch *FindBatch(const FRenderStateSnapshot &key, EBatchKind kind);
	void MakeKey(FRenderStateSnapshot &key, EBatchKind kind, int dt, int first, int count, bool indexed) const;
	void FillInstanceValues(FInstanceGPU &inst) const;

public:
	void SaveState(FRenderStateSnapshot &s) const;
	void RestoreState(const FRenderStateSnapshot &s);

	void BeginBatchScope(EBatchScope scope);
	void EndBatchScope();
	void FlushBatches(EBatchBreak reason);
	bool HasPendingBatches() const { return mPendingBatches > 0; }

	// Batchable submissions. Without an active scope or backend support they draw immediately.
	void DrawQuad(int vertexindex, const FVector3 *corners, float ul, float ur, float vt, float vb, EBatchCategory category);
	void DrawModel(int dt, int index, int count, bool indexed);
	void DrawMerged(int dt, int index, int count, bool indexed, EBatchCategory category);

	// Triangulates the items of a merged batch into 'out' (batch.mergedIndices entries).
	// 'sourceIndices' is the CPU copy of the index buffer used by indexed items.
	static void BuildMergedIndices(const FDrawBatch &batch, const uint32_t *sourceIndices, unsigned sourceCount, uint32_t *out);

public:

	uint64_t firstFrame = 0;
	VSMatrix mModelMatrix;
	VSMatrix mTextureMatrix;

public:

	void Reset()
	{
		mTextureEnabled = true;
		mBrightmapEnabled = mGradientEnabled = mFogEnabled = mGlowEnabled = false;
		mFogColor = 0xffffffff;
		mStreamData.uFogColor = mFogColor;
		mTextureMode = -1;
		mTextureClamp = 0;
		mTextureModeFlags = 0;
		mStreamData.uDesaturationFactor = 0.0f;
		mAlphaThreshold = 0.5f;
		mModelMatrixEnabled = false;
		mTextureMatrixEnabled = false;
		mSplitEnabled = false;
		mStreamData.uAddColor = 0;
		mStreamData.uObjectColor = 0xffffffff;
		mStreamData.uObjectColor2 = 0;
		mStreamData.uTextureBlendColor = 0;
		mStreamData.uTextureAddColor = 0;
		mStreamData.uTextureModulateColor = 0;
		mSoftLight = 0;
		mLightParms[0] = mLightParms[1] = mLightParms[2] = 0.0f;
		mLightParms[3] = -1.f;
		mSpecialEffect = EFF_NONE;
		mLightIndex = -1;
		mBoneIndexBase = -1;
		mStreamData.uInterpolationFactor = 0;
		mRenderStyle = DefaultRenderStyle();
		mMaterial.Reset();
		mBias.Reset();
		mPassType = NORMAL_PASS;

		mColorMapSpecial = 0;
		mColorMapFlash = 1;

		mVertexBuffer = nullptr;
		mVertexOffsets[0] = mVertexOffsets[1] = 0;
		mIndexBuffer = nullptr;

		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGradientTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGradientBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uDynLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		mStreamData.uDetailParms = { 0.0f, 0.0f, 0.0f, 0.0f };
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { 0,0,0,0 };
#endif
		mModelMatrix.loadIdentity();
		mTextureMatrix.loadIdentity();
		ClearClipSplit();
	}

	void SetNormal(FVector3 norm)
	{
		mStreamData.uVertexNormal = { norm.X, norm.Y, norm.Z, 0.f };
	}

	void SetNormal(float x, float y, float z)
	{
		mStreamData.uVertexNormal = { x, y, z, 0.f };
	}

	void SetColor(float r, float g, float b, float a = 1.f, int desat = 0)
	{
		mStreamData.uVertexColor = { r, g, b, a };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColor(PalEntry pe, int desat = 0)
	{
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, pe.a * scale };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColorAlpha(PalEntry pe, float alpha = 1.f, int desat = 0)
	{
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, alpha };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void ResetColor()
	{
		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uDesaturationFactor = 0.0f;
	}

	void SetTextureClamp(bool on)
	{
		if (on) mTextureClamp = TM_CLAMPY;
		else mTextureClamp = 0;
	}

	void SetTextureMode(int mode)
	{
		mTextureMode = mode;
	}

	void SetTextureMode(FRenderStyle style)
	{
		if (style.Flags & STYLEF_RedIsAlpha)
		{
			SetTextureMode(TM_ALPHATEXTURE);
		}
		else if (style.Flags & STYLEF_ColorIsFixed)
		{
			SetTextureMode(TM_STENCIL);
		}
		else if (style.Flags & STYLEF_InvertSource)
		{
			SetTextureMode(TM_INVERSE);
		}
	}

	int GetTextureMode()
	{
		return mTextureMode;
	}

	int GetTextureModeAndFlags(int tempTM)
	{
		int f = mTextureModeFlags;
		if (!mBrightmapEnabled) f &= ~(TEXF_Brightmap | TEXF_Glowmap);
		if (mTextureClamp) f |= TEXF_ClampY;
		return (mTextureMode == TM_NORMAL && tempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode) | f;
	}

	void EnableTexture(bool on)
	{
		mTextureEnabled = on;
	}

	void EnableFog(uint8_t on)
	{
		mFogEnabled = on;
	}

	void SetEffect(int eff)
	{
		mSpecialEffect = eff;
	}

	void EnableGlow(bool on)
	{
		if (mGlowEnabled && !on)
		{
			mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mGlowEnabled = on;
	}

	void EnableGradient(bool on)
	{
		mGradientEnabled = on;
	}

	void EnableBrightmap(bool on)
	{
		mBrightmapEnabled = on;
	}

	void EnableSplit(bool on)
	{
		if (mSplitEnabled && !on)
		{
			mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mSplitEnabled = on;
	}

	void EnableModelMatrix(bool on)
	{
		mModelMatrixEnabled = on;
	}

	void EnableTextureMatrix(bool on)
	{
		mTextureMatrixEnabled = on;
	}

	void SetGlowParams(float *t, float *b)
	{
		mStreamData.uGlowTopColor = { t[0], t[1], t[2], t[3] };
		mStreamData.uGlowBottomColor = { b[0], b[1], b[2], b[3] };
	}

	void SetSoftLightLevel(int llevel, int blendfactor = 0)
	{
		if (blendfactor == 0) mLightParms[3] = llevel / 255.f;
		else mLightParms[3] = -1.f;
	}

	void SetNoSoftLightLevel()
	{
		 mLightParms[3] = -1.f;
	}

	void SetGlowPlanes(const FVector4 &tp, const FVector4& bp)
	{
		mStreamData.uGlowTopPlane = tp;
		mStreamData.uGlowBottomPlane = bp;
	}

	void SetGradientPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uGradientTopPlane = tp;
		mStreamData.uGradientBottomPlane = bp;
	}

	void SetSplitPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uSplitTopPlane = tp;
		mStreamData.uSplitBottomPlane = bp;
	}

	void SetDetailParms(float xscale, float yscale, float bias)
	{
		mStreamData.uDetailParms = { xscale, yscale, bias, 0 };
	}

	void SetDynLight(float r, float g, float b)
	{
		mStreamData.uDynLightColor.X = r;
		mStreamData.uDynLightColor.Y = g;
		mStreamData.uDynLightColor.Z = b;
	}

	void SetScreenFade(float f)
	{
		// This component is otherwise unused.
		mStreamData.uDynLightColor.W = f;
	}

	void SetObjectColor(PalEntry pe)
	{
		mStreamData.uObjectColor = pe;
	}

	void SetObjectColor2(PalEntry pe)
	{
		mStreamData.uObjectColor2 = pe;
	}

	void SetAddColor(PalEntry pe)
	{
		mStreamData.uAddColor = pe;
	}

	void SetNpotEmulation(float factor, float offset)
	{
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { offset, factor, 0, 0 };
#endif
	}

	void ApplyTextureManipulation(TextureManipulation* texfx)
	{
		if (!texfx || texfx->AddColor.a == 0)
		{
			mStreamData.uTextureAddColor.a = 0;	// we only need to set the flags to 0
		}
		else
		{
			// set up the whole thing
			mStreamData.uTextureAddColor.SetIA(texfx->AddColor);
			auto pe = texfx->ModulateColor;
			mStreamData.uTextureModulateColor.SetFlt(pe.r * pe.a / 255.f, pe.g * pe.a / 255.f, pe.b * pe.a / 255.f, texfx->DesaturationFactor);
			mStreamData.uTextureBlendColor = texfx->BlendColor;
		}
	}
	void SetTextureColors(float* modColor, float* addColor, float* blendColor)
	{
		mStreamData.uTextureAddColor.SetFlt(addColor[0], addColor[1], addColor[2], addColor[3]);
		mStreamData.uTextureModulateColor.SetFlt(modColor[0], modColor[1], modColor[2], modColor[3]);
		mStreamData.uTextureBlendColor.SetFlt(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
	}

	void SetFog(PalEntry c, float d)
	{
		const float LOG2E = 1.442692f;	// = 1/log(2)
		mFogColor = c;
		mStreamData.uFogColor = mFogColor;
		if (d >= 0.0f) mLightParms[2] = d * (-LOG2E / 64000.f);
	}

	void SetLightParms(float f, float d)
	{
		mLightParms[1] = f;
		mLightParms[0] = d;
	}

	PalEntry GetFogColor() const
	{
		return mFogColor;
	}

	void AlphaFunc(int func, float thresh)
	{
		if (func == Alpha_Greater) mAlphaThreshold = thresh;
		else mAlphaThreshold = thresh - 0.001f;
	}

	void SetLightIndex(int index)
	{
		mLightIndex = index;
	}

	void SetBoneIndexBase(int index)
	{
		mBoneIndexBase = index;
	}

	void SetRenderStyle(FRenderStyle rs)
	{
		mRenderStyle = rs;
	}

	void SetRenderStyle(ERenderStyle rs)
	{
		mRenderStyle = rs;
	}

	auto GetDepthBias()
	{
		return mBias;
	}

	void SetDepthBias(float a, float b)
	{
		mBias.mChanged |= mBias.mFactor != a || mBias.mUnits != b;
		mBias.mFactor = a;
		mBias.mUnits = b;
	}

	void SetDepthBias(FDepthBiasState& bias)
	{
		SetDepthBias(bias.mFactor, bias.mUnits);
	}

	void ClearDepthBias()
	{
		mBias.mChanged |= mBias.mFactor != 0 || mBias.mUnits != 0;
		mBias.mFactor = 0;
		mBias.mUnits = 0;
	}

private:
	void SetMaterial(FMaterial *mat, int clampmode, int translation, int overrideshader, PClass * cls)
	{
		mMaterial.mMaterial = mat;
		mMaterial.mClampMode = clampmode;
		mMaterial.mTranslation = translation;
		mMaterial.mOverrideShader = overrideshader;
		mMaterial.mChanged = true;
		mTextureModeFlags = mat->GetLayerFlags();
		auto scale = mat->GetDetailScale();
		mStreamData.uDetailParms = { scale.X, scale.Y, 2, 0 };
	}

public:
	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, int translation, int overrideshader, PClass *cls)
	{
		tex->setSeen();
		if (!sysCallbacks.PreBindTexture || !sysCallbacks.PreBindTexture(this, tex, upscalemask, scaleflags, clampmode, translation, overrideshader))
		{
			if (shouldUpscale(tex, upscalemask)) scaleflags |= CTF_Upscale;
		}
		auto mat = FMaterial::ValidateTexture(tex, scaleflags);
		assert(mat);
		SetMaterial(mat, clampmode, translation, overrideshader, cls);
	}

	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, FTranslationID translation, int overrideshader, PClass *cls)
	{
		SetMaterial(tex, upscalemask, scaleflags, clampmode, translation.index(), overrideshader, cls);
	}


	void SetClipSplit(float bottom, float top)
	{
		mClipSplit[0] = bottom;
		mClipSplit[1] = top;
	}

	void SetClipSplit(float *vals)
	{
		memcpy(mClipSplit, vals, 2 * sizeof(float));
	}

	void GetClipSplit(float *out)
	{
		memcpy(out, mClipSplit, 2 * sizeof(float));
	}

	void ClearClipSplit()
	{
		mClipSplit[0] = -1000000.f;
		mClipSplit[1] = 1000000.f;
	}

	void SetVertexBuffer(IVertexBuffer *vb, int offset0, int offset1)
	{
		assert(vb);
		mVertexBuffer = vb;
		mVertexOffsets[0] = offset0;
		mVertexOffsets[1] = offset1;
	}

	void SetIndexBuffer(IIndexBuffer *ib)
	{
		mIndexBuffer = ib;
	}

	template <class T> void SetVertexBuffer(T *buffer)
	{
		auto ptrs = buffer->GetBufferObjects();
		SetVertexBuffer(ptrs.first, 0, 0);
		SetIndexBuffer(ptrs.second);
	}

	void SetInterpolationFactor(float fac)
	{
		mStreamData.uInterpolationFactor = fac;
	}

	float GetInterpolationFactor()
	{
		return mStreamData.uInterpolationFactor;
	}

	void EnableDrawBufferAttachments(bool on) // Used by fog boundary drawer
	{
		EnableDrawBuffers(on ? GetPassDrawBufferCount() : 1);
	}

	int GetPassDrawBufferCount()
	{
		return mPassType == GBUFFER_PASS ? 3 : 1;
	}

	void SetPassType(EPassType passType)
	{
		mPassType = passType;
	}

	EPassType GetPassType()
	{
		return mPassType;
	}

	void SetSpecialColormap(int cm, float flash)
	{
		mColorMapSpecial = cm;
		mColorMapFlash = flash;
	}

	// API-dependent render interface

	// Draw commands
	virtual void ClearScreen() = 0;
	virtual void Draw(int dt, int index, int count, bool apply = true) = 0;
	virtual void DrawIndexed(int dt, int index, int count, bool apply = true) = 0;

	// Immediate render state change commands. These only change infrequently and should not clutter the render state.
	virtual bool SetDepthClamp(bool on) = 0;					// Deactivated only by skyboxes.
	virtual void SetDepthMask(bool on) = 0;						// Used by decals and indirectly by portal setup.
	virtual void SetDepthFunc(int func) = 0;					// Used by models, portals and mirror surfaces.
	virtual void SetDepthRange(float min, float max) = 0;		// Used by portal setup.
	virtual void SetColorMask(bool r, bool g, bool b, bool a) = 0;	// Used by portals.
	virtual void SetStencil(int offs, int op, int flags=-1) = 0;	// Used by portal setup and render hacks.
	virtual void SetCulling(int mode) = 0;						// Used by model drawer only.
	virtual void EnableClipDistance(int num, bool state) = 0;	// Use by sprite sorter for vertical splits.
	virtual void Clear(int targets) = 0;						// not used during normal rendering
	virtual void EnableStencil(bool on) = 0;					// always on for 3D, always off for 2D
	virtual void SetScissor(int x, int y, int w, int h) = 0;	// constant for 3D, changes for 2D
	virtual void SetViewport(int x, int y, int w, int h) = 0;	// constant for all 3D and all 2D
	virtual void EnableDepthTest(bool on) = 0;					// used by 2D, portals and render hacks.
	virtual void EnableMultisampling(bool on) = 0;				// only active for 2D
	virtual void EnableLineSmooth(bool on) = 0;					// constant setting for each 2D drawer operation
	virtual void EnableDrawBuffers(int count, bool apply = false) = 0;	// Used by SSAO and EnableDrawBufferAttachments

	void SetColorMask(bool on)
	{
		SetColorMask(on, on, on, on);
	}

};
