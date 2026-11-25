/*
** hw_ihwtexture.h
**
** Interface for hardware textures
**
**---------------------------------------------------------------------------
**
** Copyright 2006-2020 Christoph Oelckers
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

#include <stdint.h>
#include "tarray.h"

typedef TMap<int, bool> SpriteHits;
class FTexture;

class IHardwareTexture
{
public:
	enum
	{
		MAX_TEXTURES = 16
	};

	enum HardwareState
	{
		NONE		= 0,	// Uninitialized
		LOADING		= 1,	// Waiting on texture load op
		CACHING		= 2,	// Loading with low priority
		UPLOADING	= 3,	// Waiting on transfer op
		READY		= 4,	// Fully ready to render
		FAILED		= 5		// Failed to load, unreadable etc.
	};

	enum CompressionFormat
	{
		COMPRESSION_NONE,
		COMPRESSION_BC7_UNORM,
	};

	IHardwareTexture() = default;
	virtual ~IHardwareTexture() = default;

	virtual HardwareState GetState(int texUnit = 0) { return hwState; }
	virtual void SetHardwareState(HardwareState hws, int texUnit = 0) { hwState = hws; }

	virtual void AllocateBuffer(int w, int h, int texelsize) = 0;
	virtual uint8_t *MapBuffer() = 0;
	virtual unsigned int CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name, bool noGLSwizzle = false) = 0;
	virtual unsigned int CreateCompressedTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, CompressionFormat format, const char* name) = 0;
	virtual void Finalize() = 0;

	// @Cockatrice - Used to determine if the texture is available to be used in rendering (loaded/uploaded)
	virtual bool IsValid(int texUnit = 0) { return GetState(texUnit) == HardwareState::READY; }

	void Resize(int swidth, int sheight, int width, int height, unsigned char *src_data, unsigned char *dst_data);

	int GetBufferPitch() const { return bufferpitch; }

protected:
	int bufferpitch = -1;
	HardwareState hwState = NONE;
};
