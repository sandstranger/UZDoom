/*
** hw_batching.cpp
** Draw batching and GPU instancing of sprites, particles, models, walls and flats.
**
** The scene code hands batchable geometry to FRenderState::DrawQuad/DrawModel/DrawMerged.
** Inside a batch scope (opened by the draw lists) every submission is filed into an open
** batch whose key - the complete render state with the per-instance values cleared - is
** identical. Nothing is drawn until the batch is flushed: on a state change (ordered scope),
** before any other draw or immediate state change, at the end of the scope, or when the
** batch is full. A flush restores the state captured with the first submission and draws
**   - quads/models: one glDraw*Instanced with the per-instance values in an instance buffer,
**   - merged walls/flats: one glDrawElements over the concatenated, triangulated geometry,
** or, below the threshold (r_instancing_minbatch), replays the original draws with their
** original per-instance values, which is exactly what the scene code would have drawn.
** Backends without support (BatchingSupported() == false) never see a batch.
*/

#include <chrono>
#include <math.h>

#include "hw_batching.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "v_video.h"
#include "flatvertices.h"

#ifdef __ANDROID__
#define BATCHING_DEFAULT true
#else
#define BATCHING_DEFAULT false	// desktop GL/Vulkan behaviour stays unchanged unless enabled
#endif

CVAR(Bool, r_instancing, BATCHING_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_instancing_sprites, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_instancing_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_instancing_models, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_batch_walls, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_batch_flats, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// A batch of one saves no draw call but costs an upload and a program switch.
CVAR(Int, r_instancing_minbatch, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Instances per draw (128 bytes each); bounds the per-batch upload and the CPU staging memory.
CVAR(Int, r_instancing_maxbatch, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_batch_stats, false, 0)

FBatchStats batchStats;
FBatchStats batchStatsLast;
FRenderTargetStats rtStatsGL;
FRenderTargetStats rtStatsGLES;

bool BatchingEnabled()
{
	return r_instancing;
}

bool BatchCategoryEnabled(EBatchCategory category)
{
	if (!r_instancing) return false;
	switch (category)
	{
	case BC_Sprite:		return r_instancing_sprites;
	case BC_Particle:	return r_instancing_particles;
	case BC_Model:		return r_instancing_models;
	case BC_Wall:		return r_batch_walls;
	case BC_Flat:		return r_batch_flats;
	default:			return false;
	}
}

unsigned BatchMinInstances()
{
	return (unsigned)clamp<int>(r_instancing_minbatch, 1, 4096);
}

unsigned BatchMaxInstances()
{
	return (unsigned)clamp<int>(r_instancing_maxbatch, 2, 4096);
}

bool BatchStatsTiming()
{
	return r_batch_stats;
}

double BatchTimeMs()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void BatchStatsEndFrame()
{
	batchStatsLast = batchStats;
	memset(&batchStats, 0, sizeof(batchStats));
	for (auto rt : { &rtStatsGL, &rtStatsGLES })
	{
		rt->frameFboBinds = rt->fboBinds - rt->lastFboBinds;
		rt->frameResolves = rt->resolves - rt->lastResolves;
		rt->lastFboBinds = rt->fboBinds;
		rt->lastResolves = rt->resolves;
	}
}

CCMD(r_rt_stats)
{
	static const char *names[2] = { "OpenGL renderer", "OpenGLES renderer" };
	const FRenderTargetStats *stats[2] = { &rtStatsGL, &rtStatsGLES };
	for (int i = 0; i < 2; i++)
	{
		auto &s = *stats[i];
		if (s.created == 0 && s.fboBinds == 0) continue;
		Printf("%s render targets:\n", names[i]);
		Printf("  memory %llu KB (peak %llu KB), unused in pool %llu KB\n", (unsigned long long)(s.bytes / 1024),
			(unsigned long long)(s.peakBytes / 1024), (unsigned long long)(s.pooledFreeBytes / 1024));
		Printf("  created %u, deleted %u, pool hits %u, misses %u\n", s.created, s.deleted, s.poolHits, s.poolMisses);
		Printf("  last frame: %u frame buffer binds, %u MSAA resolves\n", s.frameFboBinds, s.frameResolves);
		if (s.msaaSamples > 1) Printf("  MSAA %dx: %s\n", s.msaaSamples, s.msaaPath ? s.msaaPath : "");
		else Printf("  MSAA off\n");
	}
}

//==========================================================================
//
// State snapshots
//
//==========================================================================

static uint32_t PackColor(const FVector4PalEntry &c)
{
	auto b = [](float v) { return (uint32_t)clamp<long>(lrintf(v * 255.f), 0, 255); };
	return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24);
}

static uint32_t PackColor(PalEntry c)
{
	return c.r | (c.g << 8) | (c.b << 16) | ((uint32_t)c.a << 24);
}

static PalEntry UnpackColor(uint32_t c)
{
	return PalEntry(uint8_t(c >> 24), uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16));
}

void FRenderState::SaveState(FRenderStateSnapshot &s) const
{
	memset(&s, 0, sizeof(s));
	s.material = mMaterial.mMaterial;
	s.vertexBuffer = mVertexBuffer;
	s.indexBuffer = mIndexBuffer;
	memcpy(&s.streamData, &mStreamData, sizeof(StreamData));
	memcpy(s.modelMatrix, mModelMatrix.mMatrix, sizeof(s.modelMatrix));
	memcpy(s.textureMatrix, mTextureMatrix.mMatrix, sizeof(s.textureMatrix));
	memcpy(s.lightParms, mLightParms, sizeof(s.lightParms));
	memcpy(s.clipSplit, mClipSplit, sizeof(s.clipSplit));
	s.alphaThreshold = mAlphaThreshold;
	s.colorMapFlash = mColorMapFlash;
	s.biasFactor = mBias.mFactor;
	s.biasUnits = mBias.mUnits;
	s.lightIndex = mLightIndex;
	s.boneIndexBase = mBoneIndexBase;
	s.specialEffect = mSpecialEffect;
	s.textureMode = mTextureMode;
	s.textureClamp = mTextureClamp;
	s.textureModeFlags = mTextureModeFlags;
	s.softLight = mSoftLight;
	s.colorMapSpecial = mColorMapSpecial;
	s.passType = mPassType;
	s.clampMode = mMaterial.mClampMode;
	s.translation = mMaterial.mTranslation;
	s.overrideShader = mMaterial.mOverrideShader;
	s.vertexOffsets[0] = mVertexOffsets[0];
	s.vertexOffsets[1] = mVertexOffsets[1];
	s.fogColor = mFogColor.d;
	s.renderStyle = mRenderStyle.AsDWORD;
	s.flags = mFogEnabled | (mTextureEnabled << 8) | (mGlowEnabled << 9) | (mGradientEnabled << 10) | (mModelMatrixEnabled << 11) |
		(mTextureMatrixEnabled << 12) | (mSplitEnabled << 13) | (mBrightmapEnabled << 14);
	s.backendState = GetBackendState();
}

void FRenderState::RestoreState(const FRenderStateSnapshot &s)
{
	mMaterial.mMaterial = s.material;
	mMaterial.mClampMode = s.clampMode;
	mMaterial.mTranslation = s.translation;
	mMaterial.mOverrideShader = s.overrideShader;
	// The GL texture bindings may belong to another material now; ApplyMaterial skips the
	// actual rebinding when they don't.
	mMaterial.mChanged = s.material != nullptr;
	mVertexBuffer = s.vertexBuffer;
	mIndexBuffer = s.indexBuffer;
	memcpy(&mStreamData, &s.streamData, sizeof(StreamData));
	memcpy(mModelMatrix.mMatrix, s.modelMatrix, sizeof(s.modelMatrix));
	memcpy(mTextureMatrix.mMatrix, s.textureMatrix, sizeof(s.textureMatrix));
	memcpy(mLightParms, s.lightParms, sizeof(s.lightParms));
	memcpy(mClipSplit, s.clipSplit, sizeof(s.clipSplit));
	mAlphaThreshold = s.alphaThreshold;
	mColorMapFlash = s.colorMapFlash;
	mBias.mFactor = s.biasFactor;
	mBias.mUnits = s.biasUnits;
	mBias.mChanged = true;
	mLightIndex = s.lightIndex;
	mBoneIndexBase = s.boneIndexBase;
	mSpecialEffect = s.specialEffect;
	mTextureMode = s.textureMode;
	mTextureClamp = s.textureClamp;
	mTextureModeFlags = s.textureModeFlags;
	mSoftLight = s.softLight;
	mColorMapSpecial = s.colorMapSpecial;
	mPassType = (EPassType)s.passType;
	mVertexOffsets[0] = s.vertexOffsets[0];
	mVertexOffsets[1] = s.vertexOffsets[1];
	mFogColor.d = s.fogColor;
	mRenderStyle.AsDWORD = s.renderStyle;
	mFogEnabled = uint8_t(s.flags & 255);
	mTextureEnabled = (s.flags >> 8) & 1;
	mGlowEnabled = (s.flags >> 9) & 1;
	mGradientEnabled = (s.flags >> 10) & 1;
	mModelMatrixEnabled = (s.flags >> 11) & 1;
	mTextureMatrixEnabled = (s.flags >> 12) & 1;
	mSplitEnabled = (s.flags >> 13) & 1;
	mBrightmapEnabled = (s.flags >> 14) & 1;
	SetBackendState(s.backendState);
}

void FRenderState::MakeKey(FRenderStateSnapshot &key, EBatchKind kind, int dt, int first, int count, bool indexed) const
{
	SaveState(key);
	key.kind = kind;
	key.drawType = dt;
	key.first = first;
	key.count = count;
	key.indexed = indexed;
	if (!mTextureMatrixEnabled) memset(key.textureMatrix, 0, sizeof(key.textureMatrix));
	if (!mModelMatrixEnabled || kind == BK_Model) memset(key.modelMatrix, 0, sizeof(key.modelMatrix));

	if (kind != BK_Merge)
	{
		// Per-instance values. What they select in the shaders (fog colour present,
		// desaturation, software light level) must still match within a batch.
		key.derived = ((mFogColor.d & 0xffffff) != 0 ? 1 : 0) |
			(mStreamData.uDesaturationFactor != 0.f ? 2 : 0) |
			(mLightParms[3] >= 0.f ? 4 : 0);
		auto &sd = key.streamData;
		sd.uVertexColor = { 0, 0, 0, 0 };
		sd.uDesaturationFactor = 0;
		sd.uObjectColor.SetFlt(0, 0, 0, 0);
		sd.uAddColor.SetFlt(0, 0, 0, 0);
		sd.uFogColor.SetFlt(0, 0, 0, 0);
		sd.uDynLightColor = { 0, 0, 0, 0 };
		key.fogColor = 0;
		memset(key.lightParms, 0, sizeof(key.lightParms));
	}
}

void FRenderState::FillInstanceValues(FInstanceGPU &inst) const
{
	memcpy(inst.color, &mStreamData.uVertexColor, sizeof(inst.color));
	memcpy(inst.lightAttr, mLightParms, sizeof(inst.lightAttr));
	memcpy(inst.dynLight, &mStreamData.uDynLightColor, sizeof(inst.dynLight));
	inst.objectColor = PackColor(mStreamData.uObjectColor);
	inst.addColor = PackColor(mStreamData.uAddColor);
	inst.fogColor = PackColor(mFogColor);
	inst.desaturation = mStreamData.uDesaturationFactor;
}

void FRenderState::ApplyInstanceValues(const FInstanceGPU &inst)
{
	memcpy(&mStreamData.uVertexColor, inst.color, sizeof(inst.color));
	memcpy(mLightParms, inst.lightAttr, sizeof(inst.lightAttr));
	memcpy(&mStreamData.uDynLightColor, inst.dynLight, sizeof(inst.dynLight));
	// the same conversion SetObjectColor/SetAddColor/SetFog use
	mStreamData.uObjectColor = UnpackColor(inst.objectColor);
	mStreamData.uAddColor = UnpackColor(inst.addColor);
	mFogColor = UnpackColor(inst.fogColor);
	mStreamData.uFogColor = mFogColor;
	mStreamData.uDesaturationFactor = inst.desaturation;
}

//==========================================================================
//
// Batch management
//
//==========================================================================

void FRenderState::BeginBatchScope(EBatchScope scope)
{
	if (mPendingBatches) FlushBatches(BB_ScopeEnd);
	mBatchScope = BatchingEnabled() ? scope : BS_None;
}

void FRenderState::EndBatchScope()
{
	if (mPendingBatches) FlushBatches(BB_ScopeEnd);
	mBatchScope = BS_None;
}

void FRenderState::FlushBatches(EBatchBreak reason)
{
	if (mFlushingBatch) return;
	while (mPendingBatches > 0)
	{
		FDrawBatch *oldest = nullptr;
		for (auto &b : mBatches)
		{
			if (b.items.Size() > 0 && (!oldest || b.serial < oldest->serial)) oldest = &b;
		}
		if (!oldest) { mPendingBatches = 0; break; }
		FlushBatch(*oldest, reason);
	}
}

FDrawBatch *FRenderState::FindBatch(const FRenderStateSnapshot &key, EBatchKind kind)
{
	FDrawBatch *slot = nullptr;
	if (mBatchScope == BS_Ordered)
	{
		// Only the most recent submission's batch may grow, anything else would reorder draws.
		FDrawBatch *last = nullptr;
		for (auto &b : mBatches)
		{
			if (b.items.Size() > 0 && (!last || b.serial > last->serial)) last = &b;
		}
		if (last && memcmp(&last->key, &key, sizeof(key)) == 0) return last;
		if (mPendingBatches) FlushBatches(BB_StateChange);
		slot = &mBatches[0];
	}
	else
	{
		FDrawBatch *oldest = nullptr;
		for (auto &b : mBatches)
		{
			if (b.items.Size() == 0)
			{
				if (!slot) slot = &b;
				continue;
			}
			if (memcmp(&b.key, &key, sizeof(key)) == 0) return &b;
			if (!oldest || b.serial < oldest->serial) oldest = &b;
		}
		if (!slot)
		{
			FlushBatch(*oldest, BB_Evicted);
			slot = oldest;
		}
	}
	slot->kind = kind;
	slot->serial = ++mBatchSerial;
	slot->mergedIndices = 0;
	memcpy(&slot->key, &key, sizeof(key));
	SaveState(slot->state);
	slot->instances.Clear();
	slot->items.Clear();
	mPendingBatches++;
	return slot;
}

void FRenderState::FlushBatch(FDrawBatch &batch, EBatchBreak reason)
{
	unsigned n = batch.items.Size();
	if (n == 0) return;

	mFlushingBatch = true;
	batchStats.breaks[reason]++;
	batchStats.batches[batch.kind]++;

	FRenderStateSnapshot current;
	SaveState(current);
	RestoreState(batch.state);

	bool batched;
	if (batch.kind == BK_Merge)
	{
		batched = n >= 2 && DrawMergedBatch(batch);
	}
	else
	{
		batched = n >= BatchMinInstances() && DrawInstancedBatch(batch);
	}

	if (batched)
	{
		batchStats.batchedItems += n;
		if (n > batchStats.maxBatch) batchStats.maxBatch = n;
		for (auto &item : batch.items) batchStats.batched[item.category]++;
	}
	else
	{
		// Replay the submissions exactly as the scene code issued them.
		for (unsigned i = 0; i < n; i++)
		{
			auto &item = batch.items[i];
			if (batch.kind != BK_Merge)
			{
				ApplyInstanceValues(batch.instances[i]);
				if (batch.kind == BK_Model) memcpy(mModelMatrix.mMatrix, batch.instances[i].xform, sizeof(float) * 16);
			}
			batchStats.replayedDraws++;
			if (item.indexed) DrawIndexed(item.drawType, item.index, item.count);
			else Draw(item.drawType, item.index, item.count);
		}
	}

	RestoreState(current);
	batch.items.Clear();
	batch.instances.Clear();
	batch.mergedIndices = 0;
	batch.kind = BK_None;
	mPendingBatches--;
	mFlushingBatch = false;
}

//==========================================================================
//
// Submissions
//
//==========================================================================

void FRenderState::DrawQuad(int vertexindex, const FVector3 *corners, float ul, float ur, float vt, float vb, EBatchCategory category)
{
	batchStats.submissions[category]++;
	// The instanced quad shader takes its transform from the corners; a model or texture
	// matrix, a special effect shader or a disabled category goes the regular way.
	if (mBatchScope == BS_None || mFlushingBatch || corners == nullptr || mModelMatrixEnabled || mSpecialEffect != EFF_NONE ||
		!BatchCategoryEnabled(category) || !BatchingSupported(BK_Quad))
	{
		Draw(DT_TriangleStrip, vertexindex, 4);
		return;
	}

	double t0 = BatchStatsTiming() ? BatchTimeMs() : 0;
	FRenderStateSnapshot key;
	MakeKey(key, BK_Quad, DT_TriangleStrip, 0, 4, false);
	FDrawBatch *batch = FindBatch(key, BK_Quad);

	FInstanceGPU &inst = batch->instances[batch->instances.Reserve(1)];
	const float uv[4] = { ul, ur, vt, vb };
	for (int i = 0; i < 4; i++)
	{
		inst.xform[i][0] = corners[i].X;
		inst.xform[i][1] = corners[i].Y;
		inst.xform[i][2] = corners[i].Z;
		inst.xform[i][3] = uv[i];
	}
	FillInstanceValues(inst);
	batch->items.Push({ DT_TriangleStrip, vertexindex, 4, 0, (int8_t)category });
	if (t0 != 0) batchStats.buildMs += BatchTimeMs() - t0;

	if (batch->items.Size() >= BatchMaxInstances()) FlushBatch(*batch, BB_Capacity);
}

void FRenderState::DrawModel(int dt, int index, int count, bool indexed)
{
	batchStats.submissions[BC_Model]++;
	if (mBatchScope == BS_None || mFlushingBatch || !mModelMatrixEnabled || mSpecialEffect != EFF_NONE ||
		!BatchCategoryEnabled(BC_Model) || !BatchingSupported(BK_Model))
	{
		if (indexed) DrawIndexed(dt, index, count);
		else Draw(dt, index, count);
		return;
	}

	double t0 = BatchStatsTiming() ? BatchTimeMs() : 0;
	FRenderStateSnapshot key;
	MakeKey(key, BK_Model, dt, index, count, indexed);
	FDrawBatch *batch = FindBatch(key, BK_Model);

	FInstanceGPU &inst = batch->instances[batch->instances.Reserve(1)];
	memcpy(inst.xform, mModelMatrix.mMatrix, sizeof(inst.xform));
	FillInstanceValues(inst);
	batch->items.Push({ dt, index, count, (int8_t)indexed, (int8_t)BC_Model });
	if (t0 != 0) batchStats.buildMs += BatchTimeMs() - t0;

	if (batch->items.Size() >= BatchMaxInstances()) FlushBatch(*batch, BB_Capacity);
}

static unsigned MergedIndexCount(int dt, int count, bool indexed)
{
	if (indexed) return dt == DT_Triangles ? unsigned(count - count % 3) : 0;
	switch (dt)
	{
	case DT_Triangles:		return unsigned(count - count % 3);
	case DT_TriangleFan:
	case DT_TriangleStrip:	return count >= 3 ? unsigned(count - 2) * 3 : 0;
	default:				return 0;
	}
}

void FRenderState::DrawMerged(int dt, int index, int count, bool indexed, EBatchCategory category)
{
	batchStats.submissions[category]++;
	unsigned numIndices = MergedIndexCount(dt, count, indexed);
	bool ok = mBatchScope != BS_None && !mFlushingBatch && numIndices > 0 && index >= 0 && numIndices <= BATCH_MAX_MERGED_INDICES &&
		BatchCategoryEnabled(category) && BatchingSupported(BK_Merge);
	if (ok && indexed)
	{
		// Indexed geometry is re-indexed from the CPU copy of the level's index buffer.
		auto vd = screen->mVertexData;
		ok = vd && mIndexBuffer == vd->GetBufferObjects().second && unsigned(index) + unsigned(count) <= vd->ibo_data.Size();
	}
	if (!ok)
	{
		if (indexed) DrawIndexed(dt, index, count);
		else Draw(dt, index, count);
		return;
	}

	double t0 = BatchStatsTiming() ? BatchTimeMs() : 0;
	FRenderStateSnapshot key;
	MakeKey(key, BK_Merge, 0, 0, 0, false);
	FDrawBatch *batch = FindBatch(key, BK_Merge);
	if (batch->mergedIndices + numIndices > BATCH_MAX_MERGED_INDICES)
	{
		FlushBatch(*batch, BB_Capacity);
		batch = FindBatch(key, BK_Merge);
	}
	batch->items.Push({ dt, index, count, (int8_t)indexed, (int8_t)category });
	batch->mergedIndices += numIndices;
	if (t0 != 0) batchStats.buildMs += BatchTimeMs() - t0;
}

void FRenderState::BuildMergedIndices(const FDrawBatch &batch, const uint32_t *sourceIndices, unsigned sourceCount, uint32_t *out)
{
	for (auto &item : batch.items)
	{
		const uint32_t first = (uint32_t)item.index;
		const unsigned n = MergedIndexCount(item.drawType, item.count, item.indexed);
		if (item.indexed)
		{
			if (first + n <= sourceCount)
				memcpy(out, sourceIndices + first, n * sizeof(uint32_t));
			else
				memset(out, 0, n * sizeof(uint32_t));	// cannot happen, DrawMerged checks the range
			out += n;
		}
		else if (item.drawType == DT_Triangles)
		{
			for (unsigned i = 0; i < n; i++) *out++ = first + i;
		}
		else if (item.drawType == DT_TriangleFan)
		{
			for (int i = 1; i + 1 < item.count; i++)
			{
				*out++ = first;
				*out++ = first + i;
				*out++ = first + i + 1;
			}
		}
		else // strip: keep the winding of every other triangle as GL does
		{
			for (int i = 0; i + 2 < item.count; i++)
			{
				if (i & 1)
				{
					*out++ = first + i + 1;
					*out++ = first + i;
				}
				else
				{
					*out++ = first + i;
					*out++ = first + i + 1;
				}
				*out++ = first + i + 2;
			}
		}
	}
}

//==========================================================================
//
// Statistics
//
//==========================================================================

CCMD(r_batch_stats_print)
{
	const auto &s = batchStatsLast;
	static const char *cats[BC_Count] = { "sprites", "particles", "models", "walls", "flats" };
	static const char *kinds[4] = { "-", "quad", "model", "merge" };
	static const char *reasons[BB_Count] = { "state change", "other draw", "immediate state", "capacity", "scope end", "evicted" };

	Printf("Draw batching (%s), last frame:\n", r_instancing ? "on" : "off");
	unsigned total = s.draws + s.instancedDraws + s.mergedDraws;
	Printf("  draw calls: %u total = %u regular (%u replayed below threshold) + %u instanced + %u merged\n",
		total, s.draws, s.replayedDraws, s.instancedDraws, s.mergedDraws);
	for (int c = 0; c < BC_Count; c++)
		Printf("  %-9s: %u submitted, %u drawn in batches\n", cats[c], s.submissions[c], s.batched[c]);
	unsigned nb = s.batches[BK_Quad] + s.batches[BK_Model] + s.batches[BK_Merge];
	unsigned drawnBatches = s.instancedDraws + s.mergedDraws;
	Printf("  batches flushed: %u (%s %u, %s %u, %s %u); drawn as one call: %u, average size %.2f, max %u\n",
		nb, kinds[1], s.batches[BK_Quad], kinds[2], s.batches[BK_Model], kinds[3], s.batches[BK_Merge],
		drawnBatches, drawnBatches ? double(s.batchedItems) / drawnBatches : 0.0, s.maxBatch);
	for (int r = 0; r < BB_Count; r++)
		if (s.breaks[r]) Printf("  batch ended by %s: %u\n", reasons[r], s.breaks[r]);
	Printf("  shader switches %u, texture switches %u, vertex/index buffer binds %u\n", s.shaderSwitches, s.textureSwitches, s.bufferBinds);
	Printf("  instance/index uploads %u, %llu bytes\n", s.instanceUploads, (unsigned long long)s.uploadBytes);
	if (r_batch_stats)
		Printf("  CPU: grouping %.3f ms, building+uploading %.3f ms\n", s.buildMs, s.uploadMs);
	else
		Printf("  (set r_batch_stats 1 to measure CPU time)\n");
}
