/*
** gl_hwtexture.h
**
** GL texture abstraction
**
**---------------------------------------------------------------------------
**
** Copyright 2019 Christoph Oelckers
** Copyright 2019-2025 GZDoom Maintainers and Contributors
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
class FBitmap;
class FTexture;

#include <mutex>
#include <atomic>

#include "tarray.h"
#include "hw_ihwtexture.h"


#ifdef LoadImage
#undef LoadImage
#endif

#define SHADED_TEXTURE -1
#define DIRECT_PALETTE -2

#include "tarray.h"
#include "gl_interface.h"
#include "hw_ihwtexture.h"

class FCanvasTexture;

namespace OpenGLRenderer
{

// A BC compressed image the GPU cannot take transcoded in place to ETC2 (etcpak): returns the format to upload it in
int PrepareCompressedUpload(unsigned char* buffer, size_t totalSize, int w, int h, int numMips, int format);

class FHardwareTexture : public IHardwareTexture
{
public:
	

	static unsigned int lastbound[MAX_TEXTURES];

	// Live texture objects and the bytes of their uploads (estimated: 4 bytes a texel, mipmaps 1/3 more)
	static size_t sLiveTextures;
	static size_t sTextureBytes;
	static size_t sTextureUploads;	// CreateTexture calls (first uploads and updates)

	static int GetTexDimension(int value)
	{
		if (value > gl.max_texturesize) return gl.max_texturesize;
		return value;
	}

	static void InitGlobalState() { for (int i = 0; i < MAX_TEXTURES; i++) lastbound[i] = 0; }

private:
	struct GLLoadInfo {
		bool forcenofilter = false;
		unsigned int glTexID = 0;
		int glTextureBytes = 0;
		bool mipmapped = false;
	} glInfo; // Used to manage shared information between threads when loading

	bool forcenofilter;

	unsigned int glTexID = 0;
	unsigned int glBufferID = 0;
	size_t mTrackedBytes = 0;	// counted in sTextureBytes
	int glTextureBytes;
	bool mipmapped = false;
#ifdef ANDROID
    int size;
#endif

public:
	FHardwareTexture(int numchannels = 4, bool disablefilter = false)
	{
		forcenofilter = disablefilter;
		glTextureBytes = numchannels;
	}

	~FHardwareTexture();

	static void Unbind(int texunit);
	static void UnbindAll();

	void BindToFrameBuffer(int w, int h);

	unsigned int Bind(int texunit, bool needmipmap);
	bool BindOrCreate(FTexture* tex, int texunit, int clampmode, int translation, int flags, bool noMips = false);

	void AllocateBuffer(int w, int h, int texelsize);
	uint8_t* MapBuffer();

	unsigned int CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, const char* name, bool noGLSwizzle = false);
	unsigned int CreateCompressedTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, CompressionFormat format, const char* name) override;
	void Finalize() { }
	unsigned int BackgroundCreateTexture(GLsync* fence, unsigned char* buffer, int w, int h, int texunit, bool mipmap, bool indexed, const char* name, bool forceNoMips = false);
	unsigned int BackgroundCreateCompressedTexture(GLsync *fence, unsigned char* buffer, uint32_t dataSize, uint32_t totalSize, int w, int h, int format, int texunit, int numMips, const char* name, bool forceNoMips = false, bool allowQualityReduction = false);
	unsigned int CreateCompressedTexture(unsigned char* buffer, uint32_t dataSize, uint32_t totalSize, int w, int h, int format, int texunit, int numMips, const char* name, bool forceNoMips = false, bool allowQualityReduction = false);
	bool CreateCompressedMipmap(unsigned int glTexID, unsigned char* buffer, int mipLevel, int w, int h, int format, int32_t size, int texunit);
	unsigned int GetTextureHandle()
	{
		return glTexID;
	}

	int numChannels() { return glTextureBytes; }

	bool SwapToLoadedImage();
	void CreateInvalid(int texUnit);
	void DestroyLoadedImage();
	HardwareState GetState(int texUnit) override { return hwState; }
	void SetHardwareState(HardwareState hws, int texUnit = 0) override { 
		hwState = hws;
	}
};

}
