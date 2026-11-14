/*
** hw_bonebuffer.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2014-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "tarray.h"
#include "hwrenderer/data/buffers.h"
#include "common/utility/matrix.h"
#include <atomic>
#include <mutex>

class FRenderState;

class BoneBuffer
{
	IDataBuffer *mBuffer;
	IDataBuffer* mBufferPipeline[HW_MAX_PIPELINE_BUFFERS];
	int mPipelineNbr;
	int mPipelinePos = 0;

	bool mBufferType;
	std::atomic<unsigned int> mIndex;
	unsigned int mMapStart = 0;	// mIndex at Map: what is written up to Unmap is reported to the backend
	unsigned int mBlockAlign;
	unsigned int mBlockSize;
	unsigned int mBufferSize;
	unsigned int mByteSize;
	unsigned int mMaxUploadSize;

public:
	BoneBuffer(int pipelineNbr = 1);
	~BoneBuffer();

	void Clear();
	int UploadBones(const TArray<VSMatrix> &bones);
	void Map();
	void Unmap();
	unsigned int GetBlockSize() const { return mBlockSize; }
	bool GetBufferType() const { return mBufferType; }
	int GetBinding(unsigned int index, size_t* pOffset, size_t* pSize);

	// Only for GLES to determin how much data is in the buffer
	int GetCurrentIndex() { return mIndex; };

	// OpenGL needs the buffer to mess around with the binding.
	IDataBuffer* GetBuffer() const
	{
		return mBuffer;
	}
};
