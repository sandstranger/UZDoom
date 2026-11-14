#pragma once

// Draw batching / GPU instancing: configuration and statistics shared by the scene code
// and the GL/GLES backends. The batching logic itself lives in FRenderState (hw_batching.cpp).

#include <stdint.h>
#include "hw_renderstate.h"

// Vertex attribute locations of the per-instance data (after the regular VATTR_* locations).
// Location VATTR_COLOR is fed per instance as well (FInstanceGPU::color).
enum
{
	VATTR_INSTANCE0 = 9,		// FInstanceGPU::xform[0..3] -> locations 9..12
	VATTR_INSTANCE_LIGHT = 13,	// lightAttr
	VATTR_INSTANCE_DYNLIGHT = 14,	// dynLight
	VATTR_INSTANCE_COLORS = 15,	// objectColor, addColor, fogColor, desaturation (uvec4)
	VATTR_INSTANCE_LAST = 15,
	BATCH_MIN_VERTEX_ATTRIBS = 16,	// GL_MAX_VERTEX_ATTRIBS needed (GLES 3.0 guarantees 16)
};

// Upper bound of a merged batch (indices), keeps the per-batch upload small.
constexpr unsigned BATCH_MAX_MERGED_INDICES = 48 * 1024;

struct FBatchStats
{
	// draw submissions to the GPU
	unsigned draws;				// regular draws (glDrawArrays/glDrawElements), including replays
	unsigned instancedDraws;	// glDraw*Instanced
	unsigned mergedDraws;		// merged indexed draws
	unsigned replayedDraws;		// regular draws issued for batches below the threshold

	unsigned submissions[BC_Count];	// batchable submissions by category
	unsigned batched[BC_Count];		// ...of which drawn through an instanced or merged draw
	unsigned batches[4];			// flushed batches by EBatchKind
	unsigned maxBatch;
	uint64_t batchedItems;			// sum of the sizes of instanced/merged batches
	unsigned breaks[BB_Count];

	unsigned shaderSwitches;
	unsigned textureSwitches;
	unsigned bufferBinds;
	unsigned instanceUploads;
	uint64_t uploadBytes;

	double buildMs;		// CPU time spent grouping submissions (only measured with r_batch_stats)
	double uploadMs;	// CPU time spent building and uploading instance/index data
};

extern FBatchStats batchStats;		// the frame being rendered
extern FBatchStats batchStatsLast;	// the previous complete frame

void BatchStatsEndFrame();
bool BatchStatsTiming();
bool BatchCategoryEnabled(EBatchCategory category);
bool BatchingEnabled();
unsigned BatchMinInstances();
unsigned BatchMaxInstances();
double BatchTimeMs();	// monotonic clock for the optional CPU timings
