/*
** gles_buffers.cpp
**
** Low level vertex buffer class
**
**---------------------------------------------------------------------------
**
** Copyright 2018-2020 Christoph Oelckers
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

#include <algorithm>
#include "gles_system.h"
#include "gles_buffers.h"
#include "gles_renderstate.h"
#include "v_video.h"
#include "flatvertices.h"

namespace OpenGLESRenderer
{

//==========================================================================
//
// basic buffer implementation
//
//==========================================================================

static inline void InvalidateBufferState()
{
	gl_RenderState.ResetVertexBuffer();	// force rebinding of buffers on next Apply call.
}

GLBuffer::GLBuffer(int usetype)
	: mUseType(usetype)
{
	if ((usetype == GL_ARRAY_BUFFER) || (usetype == GL_ELEMENT_ARRAY_BUFFER))
	{
		glGenBuffers(1, &mBufferId);
		isData = false;
	}
	else
	{
		isData = true;
	}
}

GLBuffer::~GLBuffer()
{
	if (mBufferId != 0)
	{
		if (mMapped)
		{
			glBindBuffer(mUseType, mBufferId);
			glUnmapBuffer(mUseType);
		}
		glBindBuffer(mUseType, 0);
		glDeleteBuffers(1, &mBufferId);
	}

	if (memory)
		delete[] memory;
}

void GLBuffer::Bind()
{
	if (!isData)
	{
		glBindBuffer(mUseType, mBufferId);
	}
}


//===========================================================================
//
// Staged vertex data: the CPU writes into a staging copy shared by the flat
// vertex data's pipeline buffers, Upload records the written ranges and Unmap
// copies them in (unsynchronized range mappings on GLES 3.0, which the pipeline
// fences protect; glBufferSubData on GLES 2.0). The GPU storage grows with what
// the frames use instead of being allocated at the full logical size, and
// there is no per-buffer CPU copy.
//
//===========================================================================

static const size_t StagedMinCapacity = size_t(1) << 20;

bool GLBuffer::SetStaging(void *staging)
{
	if (staging == nullptr || isData || mUseType != GL_ARRAY_BUFFER) return false;
	mStaging = (char*)staging;
	return true;
}

void GLBuffer::FlushStaged()
{
	std::vector<std::pair<size_t, size_t>> dirty;
	{
		std::lock_guard<std::mutex> lock(mDirtyLock);
		dirty.swap(mDirty);
	}
	if (dirty.empty()) return;

	size_t end = 0;
	for (auto &range : dirty) end = std::max(end, range.first + range.second);
	end = std::min(end, buffersize);

	Bind();
	if (end > mCapacity)
	{
		size_t capacity = std::max(std::max(end, mCapacity * 2), StagedMinCapacity);
		capacity = std::min((capacity + 0xffff) & ~size_t(0xffff), buffersize);
		glBufferData(mUseType, capacity, nullptr, GL_DYNAMIC_DRAW);
		mCapacity = capacity;
		// New storage holds nothing: all of it up to this upload goes in (the level geometry and
		// what the frame has written so far). Draws issued before keep the old storage.
		dirty.clear();
		dirty.push_back({ 0, end });
	}
	for (auto &range : dirty)
	{
		const size_t start = std::min(range.first, end);
		const size_t size = std::min(range.second, end - start);
		if (size == 0) continue;
		void *p = gles.useMappedBuffers ? glMapBufferRange(mUseType, start, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT) : nullptr;
		if (p)
		{
			memcpy(p, mStaging + start, size);
			glUnmapBuffer(mUseType);
		}
		else
		{
			glBufferSubData(mUseType, start, size, mStaging + start);
		}
	}
	InvalidateBufferState();
}

void GLBuffer::SetData(size_t size, const void* data, BufferUsageType usage)
{
	if (mStaging)
	{
		// the logical size; GPU storage is allocated by the uploads (FlushStaged)
		map = mStaging;
		buffersize = size;
		{
			std::lock_guard<std::mutex> lock(mDirtyLock);
			mDirty.clear();
			if (data)
			{
				memcpy(mStaging, data, size);
				mDirty.push_back({ 0, size });
			}
		}
		FlushStaged();
		InvalidateBufferState();
		return;
	}
	bool staticdata = (usage == BufferUsageType::Static || usage == BufferUsageType::Mappable);
	if (isData || !gles.useMappedBuffers)
	{
		if (memory)
			delete[] memory;

		memory = (char*)(new uint64_t[size / 8 + 16]);

		if (data)
			memcpy(memory, data, size);
	}

	if (!isData)
	{
		Bind();
		glBufferData(mUseType, size, data, staticdata ? GL_STATIC_DRAW : GL_STREAM_DRAW);
	}

	if (!isData && gles.useMappedBuffers)
	{
		map = 0;
	}
	else
	{
		map = memory;
	}

	buffersize = size;
	InvalidateBufferState();
}

void GLBuffer::SetSubData(size_t offset, size_t size, const void *data)
{
	Bind();

	memcpy(memory + offset, data, size);

	if (!isData)
	{
		glBufferSubData(mUseType, offset, size, data);
	}
}

void GLBuffer::Upload(size_t start, size_t size)
{
	if (mStaging)
	{
		if (size == 0) return;
		std::lock_guard<std::mutex> lock(mDirtyLock);	// plane updates may come from other threads
		mDirty.push_back({ start, size });
		return;
	}
	if (!gles.useMappedBuffers)
	{
		Bind();

		if(size)
			glBufferSubData(mUseType, start, size, memory + start);
	}
}

void GLBuffer::Map()
{
	if (mStaging)
	{
		map = mStaging;
		return;
	}
	if (!isData && gles.useMappedBuffers)
	{
		Bind();
		map = (FFlatVertex*)glMapBufferRange(mUseType, 0, buffersize, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		mMapped = map != nullptr;
	}
	else
	{
		map = memory;
	}
	InvalidateBufferState();
}

void GLBuffer::Unmap()
{
	if (mStaging)
	{
		FlushStaged();
		return;
	}
	if (!isData && gles.useMappedBuffers && mMapped)
	{
		Bind();
		glUnmapBuffer(mUseType);
		mMapped = false;
		InvalidateBufferState();
	}
}

void *GLBuffer::Lock(unsigned int size)
{
	// This initializes this buffer as a static object with no data.
	SetData(size, nullptr, BufferUsageType::Mappable);
	if (!isData && gles.useMappedBuffers)
	{
		void *p = glMapBufferRange(mUseType, 0, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		mMapped = p != nullptr;
		return p;
	}
	else
	{
		return map;
	}
}

void GLBuffer::Unlock()
{
	if (!isData)
	{
		if (gles.useMappedBuffers)
		{
			Bind();
			if (mMapped) glUnmapBuffer(mUseType);
			mMapped = false;
			InvalidateBufferState();
		}
		else
		{
			Bind();
			glBufferData(mUseType, buffersize, map, GL_STATIC_DRAW);
			InvalidateBufferState();
		}
	}
}

void GLBuffer::Resize(size_t newsize)
{
	if (newsize > buffersize)
	{
		if (isData)
		{
			// Create new bigger memory
			char* memoryNew = (char*)(new uint64_t[newsize / 8 + 16]);

			// Copy old data
			memcpy(memoryNew, memory, buffersize);

			// Delete old memory
			delete[] memory;

			memory = memoryNew;

			buffersize = newsize;
		}
		else
		{
			// TODO
		}
	}
}

void GLBuffer::GPUDropSync()
{
	if (gles.syncObjects && gles.useMappedBuffers)
	{
		if (mGLSync != NULL)
		{
			glDeleteSync(mGLSync);
		}

		mGLSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}
}

void GLBuffer::GPUWaitSync()
{
	if (gles.syncObjects && gles.useMappedBuffers && mGLSync)
	{
		GLenum status = glClientWaitSync(mGLSync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000 * 1000 * 50); // Wait for a max of 50ms...

		if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
		{
			//Printf("Error on glClientWaitSync: %d\n", status);
		}

		glDeleteSync(mGLSync);

		mGLSync = NULL;
	}
}


//===========================================================================
//
// Vertex buffer implementation
//
//===========================================================================

void GLVertexBuffer::SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	static int VFmtToGLFmt[] = { GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_INT_2_10_10_10_REV, GL_UNSIGNED_BYTE }; // TODO Fix last entry GL_INT_2_10_10_10_REV, normals for models will be broken
	static uint8_t VFmtToSize[] = {4, 3, 2, 1, 4, 4, 4};
	static bool VFmtToNormalize[] = { false, false, false, false, true, true, false };
	static bool VFmtToIntegerType[] = { false, false, false, false, false, false, true };

	mStride = stride;
	mNumBindingPoints = numBindingPoints;

	for(int i = 0; i < numAttributes; i++)
	{
		if (attrs[i].location >= 0 && attrs[i].location < VATTR_MAX)
		{
			auto & attrinf = mAttributeInfo[attrs[i].location];
			attrinf.format = VFmtToGLFmt[attrs[i].format];
			attrinf.size = VFmtToSize[attrs[i].format];
			attrinf.offset = attrs[i].offset;
			attrinf.bindingpoint = attrs[i].binding;
			attrinf.normalize = VFmtToNormalize[attrs[i].format];
			attrinf.integerType = VFmtToIntegerType[attrs[i].format];
		}
	}
}

void GLVertexBuffer::Bind(int *offsets)
{
	int i = 0;

	// This is what gets called from RenderState.Apply. It shouldn't be called anywhere else if the render state is in use
	GLBuffer::Bind();
	for(auto &attrinf : mAttributeInfo)
	{
		if (attrinf.size == 0)
		{
			glDisableVertexAttribArray(i);
		}
		else
		{
			glEnableVertexAttribArray(i);
			size_t ofs = offsets == nullptr ? attrinf.offset : attrinf.offset + mStride * offsets[attrinf.bindingpoint];
			if (!attrinf.integerType)
				glVertexAttribPointer(i, attrinf.size, attrinf.format, attrinf.normalize, (GLsizei)mStride, (void*)(intptr_t)ofs);
			else
			{
				if (gles.integerAttribs)
					glVertexAttribIPointer(i, attrinf.size, attrinf.format, (GLsizei)mStride, (void*)(intptr_t)ofs);
			}
		}
		i++;
	}
}

void GLDataBuffer::BindRange(FRenderState *state, size_t start, size_t length)
{
	if (mBindingPoint == 3)// VIEWPOINT_BINDINGPOINT
	{
		static_cast<FGLRenderState*>(state)->ApplyViewport(memory + start);
	}
}

void GLDataBuffer::BindBase()
{

}


GLVertexBuffer::GLVertexBuffer() : GLBuffer(GL_ARRAY_BUFFER) {}
GLIndexBuffer::GLIndexBuffer() : GLBuffer(GL_ELEMENT_ARRAY_BUFFER) {}
GLDataBuffer::GLDataBuffer(int bindingpoint, bool is_ssbo) : GLBuffer(0), mBindingPoint(bindingpoint) {}

}
