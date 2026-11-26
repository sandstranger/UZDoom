/*
** gles_buffers.h
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

#pragma once

#include <mutex>
#include <vector>
#include "buffers.h"

#ifdef _MSC_VER
// silence bogus warning C4250: 'GLVertexBuffer': inherits 'GLBuffer::GLBuffer::SetData' via dominance
// According to internet infos, the warning is erroneously emitted in this case.
#pragma warning(disable:4250)
#endif

namespace OpenGLESRenderer
{

class GLBuffer : virtual public IBuffer
{
protected:
	const int mUseType;
	unsigned int mBufferId = 0;
	int mAllocationSize = 0;
	bool mPersistent = false;
	bool nomap = true;
	GLsync mGLSync = 0;

	bool isData = false;
	char *memory = nullptr;
	bool mMapped = false;	// glUnmapBuffer on a buffer that is not mapped is GL_INVALID_OPERATION

	// staged vertex data (IBuffer::SetStaging)
	char *mStaging = nullptr;
	size_t mCapacity = 0;	// GPU storage of a staged buffer
	std::mutex mDirtyLock;
	std::vector<std::pair<size_t, size_t>> mDirty;	// written ranges not on the GPU yet
	void FlushStaged();

	GLBuffer(int usetype);
	~GLBuffer();
	bool SetStaging(void *staging) override;
	void SetData(size_t size, const void *data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void *data) override;
	void Map() override;
	void Unmap() override;
	void Resize(size_t newsize) override;
	void *Lock(unsigned int size) override;
	void Unlock() override;

	void GPUDropSync();
	void GPUWaitSync();
public:
	void Bind();
	void Upload(size_t start, size_t end);
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
	bool HasAttribute(int location) const { return location >= 0 && location < VATTR_MAX && mAttributeInfo[location].size != 0; }
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
