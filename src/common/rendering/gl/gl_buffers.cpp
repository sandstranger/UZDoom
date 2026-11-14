/*
** gl_buffers.cpp
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

#include <algorithm>
#include "gl_load.h"
#include "gl_buffers.h"
#include "gl_renderstate.h"
#include "v_video.h"
#include "flatvertices.h"

namespace OpenGLRenderer
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
	glGenBuffers(1, &mBufferId);
}

GLBuffer::~GLBuffer()
{
	if (mBufferId != 0)
	{
		// glUnmapBuffer on a buffer that is not mapped is GL_INVALID_OPERATION (the 2D drawer's
		// temporary buffers raised it every frame): only a mapped one is released first.
		if (map != nullptr)
		{
			glBindBuffer(mUseType, mBufferId);
			glUnmapBuffer(mUseType);
			glBindBuffer(mUseType, 0);
		}
		glDeleteBuffers(1, &mBufferId);
	}
}

void GLBuffer::Bind()
{
	glBindBuffer(mUseType, mBufferId);
}


//===========================================================================
//
// Staged vertex data (OpenGL ES, no persistent mapping): the CPU writes into a
// staging copy shared by the flat vertex data's pipeline buffers, Upload
// records the written ranges and Unmap copies them in - through unsynchronized
// range mappings, like the full-buffer mapping this replaces (the pipeline
// fences keep the GPU off a buffer that is written). The GPU storage grows with
// what the frames use instead of being allocated at the full logical size.
//
//===========================================================================

static const size_t StagedMinCapacity = size_t(1) << 20;

bool GLBuffer::SetStaging(void *staging)
{
	if (staging == nullptr || mUseType != GL_ARRAY_BUFFER || screen->BuffersArePersistent()) return false;
	mStaging = (char*)staging;
	return true;
}

void GLBuffer::Upload(size_t start, size_t size)
{
	if (!mStaging || size == 0) return;
	std::lock_guard<std::mutex> lock(mDirtyLock);	// plane updates may come from other threads
	mDirty.push_back({ start, size });
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
		void *p = glMapBufferRange(mUseType, start, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
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

void GLBuffer::SetData(size_t size, const void *data, BufferUsageType usage)
{
	if (mStaging)
	{
		// the logical size; GPU storage is allocated by the uploads (FlushStaged)
		mPersistent = false;
		nomap = false;
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
	Bind();
	if (usage == BufferUsageType::Static)
	{
		glBufferData(mUseType, size, data, GL_STATIC_DRAW);
	}
	else if (usage == BufferUsageType::Stream)
	{
		glBufferData(mUseType, size, data, GL_STREAM_DRAW);
	}
	else if (usage == BufferUsageType::Persistent)
	{
		mPersistent = screen->BuffersArePersistent();
		if (mPersistent)
		{
			glBufferStorage(mUseType, size, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
			map = glMapBufferRange(mUseType, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
		}
		else
		{
			glBufferData(mUseType, size, nullptr, GL_STREAM_DRAW);
			map = nullptr;
		}
		nomap = false;
	}
	else if (usage == BufferUsageType::Mappable)
	{
		glBufferData(mUseType, size, nullptr, GL_STREAM_DRAW);
		map = nullptr;
	}
	buffersize = size;
	InvalidateBufferState();
}

void GLBuffer::SetSubData(size_t offset, size_t size, const void *data)
{
	Bind();
	glBufferSubData(mUseType, offset, size, data);
}

void GLBuffer::Map()
{
	if (mStaging)
	{
		map = mStaging;
		return;
	}
	assert(nomap == false);	// do not allow mapping of static buffers. Vulkan cannot do that so it should be blocked in OpenGL, too.
	if (!mPersistent && !nomap)
	{
		Bind();
		map = (FFlatVertex*)glMapBufferRange(mUseType, 0, buffersize, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		InvalidateBufferState();
	}
}

void GLBuffer::Unmap()
{
	if (mStaging)
	{
		FlushStaged();
		return;
	}
	assert(nomap == false);
	if (!mPersistent && map != nullptr)
	{
		Bind();
		glUnmapBuffer(mUseType);
		InvalidateBufferState();
		map = nullptr;
	}
}

void *GLBuffer::Lock(unsigned int size)
{
	// This initializes this buffer as a static object with no data.
	SetData(size, nullptr, BufferUsageType::Mappable);
	map = glMapBufferRange(mUseType, 0, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	return map;
}

void GLBuffer::Unlock()
{
	Bind();
	glUnmapBuffer(mUseType);
	map = nullptr;
	InvalidateBufferState();
}

void GLBuffer::Resize(size_t newsize)
{
	assert(!nomap);	// only mappable buffers can be resized.
	if (newsize > buffersize && !nomap)
	{
		// reallocate the buffer with twice the size
		unsigned int oldbuffer = mBufferId;
		size_t oldsize = buffersize;

		// first unmap the old buffer (if it is mapped at all: non-persistent buffers mostly aren't)
		Bind();
		if (map != nullptr) glUnmapBuffer(mUseType);

		glGenBuffers(1, &mBufferId);
		SetData(newsize, nullptr, BufferUsageType::Persistent);
		glBindBuffer(GL_COPY_READ_BUFFER, oldbuffer);

		// copy contents and delete the old buffer.
		// SetData already changed buffersize to the new size, which is more than the old buffer holds.
		glCopyBufferSubData(GL_COPY_READ_BUFFER, mUseType, 0, 0, oldsize);
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		glDeleteBuffers(1, &oldbuffer);
		buffersize = newsize;
		InvalidateBufferState();
	}
}

void GLBuffer::GPUWaitSync()
{
	if (mGLSync != nullptr)
	{
        const GLenum status = glClientWaitSync(mGLSync, 0, 1000 * 1000 * 16);
        if (status == GL_TIMEOUT_EXPIRED) {
            glClientWaitSync(mGLSync, 0, 1000 * 1000 * 50);
        }
		glDeleteSync(mGLSync);
		mGLSync = nullptr;
	}
}

void GLBuffer::GPUDropSync()
{
	if (mGLSync != nullptr)
	{
		glDeleteSync(mGLSync);
	}

	mGLSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

//===========================================================================
//
// Vertex buffer implementation
//
//===========================================================================

void GLVertexBuffer::SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	static int VFmtToGLFmt[] = { GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_INT_2_10_10_10_REV, GL_UNSIGNED_BYTE };
	static uint8_t VFmtToSize[] = { 4, 3, 2, 1, 4, 4, 4 };
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
				glVertexAttribIPointer(i, attrinf.size, attrinf.format, (GLsizei)mStride, (void*)(intptr_t)ofs);
		}
		i++;
	}
}

void GLDataBuffer::BindRange(FRenderState *state, size_t start, size_t length)
{
	glBindBufferRange(mUseType, mBindingPoint, mBufferId, start, length);
}

void GLDataBuffer::BindBase()
{
	glBindBufferBase(mUseType, mBindingPoint, mBufferId);
}


GLVertexBuffer::GLVertexBuffer() : GLBuffer(GL_ARRAY_BUFFER) {}
GLIndexBuffer::GLIndexBuffer() : GLBuffer(GL_ELEMENT_ARRAY_BUFFER) {}
GLDataBuffer::GLDataBuffer(int bindingpoint, bool is_ssbo) : GLBuffer(is_ssbo ? GL_SHADER_STORAGE_BUFFER : GL_UNIFORM_BUFFER), mBindingPoint(bindingpoint) {}

}
