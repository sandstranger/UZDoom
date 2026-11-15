/*
** gl_buffers.h
**
** Low level vertex buffer class
**
**---------------------------------------------------------------------------
**
** Copyright 2018-2020 Christoph Oelckers
** Copyright 2018-2025 GZDoom Maintainers and Contributors
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

#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "buffers.h"
#include "gl_load.h"

#ifdef _MSC_VER
// silence bogus warning C4250: 'GLVertexBuffer': inherits 'GLBuffer::GLBuffer::SetData' via dominance
// According to internet infos, the warning is erroneously emitted in this case.
#pragma warning(disable:4250)
#endif

namespace OpenGLRenderer
{

class GLBuffer : virtual public IBuffer
{
protected:
	const int mUseType;
	unsigned int mBufferId;
	int mAllocationSize = 0;
	bool mPersistent = false;
	bool nomap = true;
	GLsync mGLSync = 0;

	// staged vertex data (IBuffer::SetStaging)
	char *mStaging = nullptr;
	size_t mCapacity = 0;	// GPU storage of a staged buffer
	size_t mTrackedBytes = 0;	// counted in sBufferBytes
	void TrackStorage(size_t bytes) { sBufferBytes = sBufferBytes - mTrackedBytes + bytes; mTrackedBytes = bytes; }
public:
	// Live buffer objects and their storage (the memory report)
	static size_t sLiveBuffers;
	static size_t sBufferBytes;
protected:
	std::mutex mDirtyLock;
	std::vector<std::pair<size_t, size_t>> mDirty;	// written ranges not on the GPU yet (staged), or not flushed yet (explicit flush)
	void FlushStaged();

	// OpenGL ES dynamic buffers: mapped with GL_MAP_FLUSH_EXPLICIT_BIT, only the ranges reported
	// through Upload() are flushed at Unmap (all of the buffer when its user reports none).
	// With GL_EXT_buffer_storage the mapping is made once and kept (mPersistentMap, not coherent).
	bool mExplicitFlush = false;
	bool mRangesReported = false;	// Upload() was called since the last Unmap
	bool mImmutable = false;		// has glBufferStorage storage: cannot be respecified
	void *mPersistentMap = nullptr;
	void FlushWritten(bool atUnmap);
	void ReleaseImmutableStorage();
	void SetPersistentRegistered(bool on);
	static void FlushPendingWritesSlow();

	GLBuffer(int usetype);
	~GLBuffer();
	bool SetStaging(void *staging) override;
	void Upload(size_t start, size_t size) override;
	void SetData(size_t size, const void *data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void *data) override;
#ifdef __ANDROID__
public:
#endif
	void Map() override;
	void Unmap() override;
#ifdef __ANDROID__
protected:
#endif
	void Resize(size_t newsize) override;
	void *Lock(unsigned int size) override;
	void Unlock() override;

	void GPUDropSync();
	void GPUWaitSync();
public:
	void Bind();

	// Persistently mapped buffers are written between Map and Unmap, but some writes are drawn
	// before the Unmap (the bones of a model are uploaded right before its draws): the draw calls
	// flush what was reported (Upload) so far. Only a relaxed flag check when nothing is pending.
	static std::atomic<bool> sWritesPending;
	static void FlushPendingWrites()
	{
		if (sWritesPending.load(std::memory_order_relaxed)) FlushPendingWritesSlow();
	}
};


class GLVertexBuffer : public IVertexBuffer, public GLBuffer
{
	// If this could use the modern (since GL 4.3) binding system, things would be simpler... :(
	struct GLVertexBufferAttribute
	{
		int bindingpoint;
		int format;
		bool normalize;
		bool integerType;
		int size;
		int offset;
	};

	int mNumBindingPoints;
	GLVertexBufferAttribute mAttributeInfo[VATTR_MAX] = {};	// Thanks to OpenGL's state system this needs to contain info about every attribute that may ever be in use throughout the entire renderer.
	size_t mStride = 0;

public:
	GLVertexBuffer();
	void SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs) override;
	void Bind(int *offsets);
};

class GLIndexBuffer : public IIndexBuffer, public GLBuffer
{
public:
	GLIndexBuffer();
};

class GLDataBuffer : public IDataBuffer, public GLBuffer
{
	int mBindingPoint;
public:
	GLDataBuffer(int bindingpoint, bool is_ssbo);
	void BindRange(FRenderState* state, size_t start, size_t length);
	void BindBase();
};

}
