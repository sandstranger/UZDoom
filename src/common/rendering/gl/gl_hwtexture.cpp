/*
** gl_hwtexture.cpp
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

#include "gl_system.h"

#include "c_cvars.h"
#include "hw_material.h"

#include "gl_interface.h"
#include "hw_cvars.h"
#include "gl_debug.h"
#include "gl_renderer.h"
#include "gl_renderstate.h"
#include "gl_samplers.h"
#include "gl_hwtexture.h"
#include "bctranscode.h"
#include "etc2cache.h"

#include "image.h"
#include "filesystem.h"

EXTERN_CVAR(Int, gl_texture_etc2_transcode)

namespace OpenGLRenderer
{

static int UploadForcedETC2(const unsigned char* bgra, int w, int h, bool mipmaps);

TexFilter_s TexFilter[] = {
	{GL_NEAREST,					GL_NEAREST,		false},
	{GL_NEAREST_MIPMAP_NEAREST,		GL_NEAREST,		true},
	{GL_LINEAR,						GL_LINEAR,		false},
	{GL_LINEAR_MIPMAP_NEAREST,		GL_LINEAR,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_LINEAR,		true},
	{GL_NEAREST_MIPMAP_LINEAR,		GL_NEAREST,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_NEAREST,		true},
};

//===========================================================================
//
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];
size_t FHardwareTexture::sLiveTextures = 0;
size_t FHardwareTexture::sTextureBytes = 0;
size_t FHardwareTexture::sTextureUploads = 0;

//===========================================================================
//
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================

unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name, bool noGLSwizzle /*= false*/)
{
	int rh,rw;
	int texformat = GL_RGBA8;// TexFormat[gl_texture_format];
	bool deletebuffer=false;
	bool noData = false;

	/*
	if (forcenocompression)
	{
		texformat = GL_RGBA8;
	}
	*/
	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
		if (glTexID != 0) sLiveTextures++;
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);

	FGLDebug::LabelObject(GL_TEXTURE, glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);
	if (glBufferID > 0)
	{
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		buffer = nullptr;
	}
	else if (!buffer)
	{
		// The texture must at least be initialized if no data is present.
		mipmapped = false;
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		noData = true;
		//texheight=-h;
	}
	else
	{
		if (rw < w || rh < h)
		{
			// The texture is larger than what the hardware can handle so scale it down.
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				Resize(w, h, rw, rh, buffer, scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}
	}
	// store the physical size.

	int sourcetype;
	if (glTextureBytes > 0)
	{
		if (glTextureBytes < 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		static const int ITypes[] = { GL_R8, GL_RG8, GL_RGB8, GL_RGBA8 };
		static const int STypes[] = { GL_RED, GL_RG, GL_BGR, GL_BGRA };
		static const int NTypes[] = { GL_RED, GL_RG, GL_RGB, GL_RGBA };

		texformat = ITypes[glTextureBytes - 1];
		sourcetype = noGLSwizzle ? NTypes[glTextureBytes - 1] : STypes[glTextureBytes - 1];
	}
	else
	{
		sourcetype = GL_BGRA;
	}

	bool swapRB = false;
	if (gl.esversion > 0)
	{
		// OpenGL ES 3.0: no GL_BGR, GL_BGRA only with EXT_texture_format_BGRA8888 (then also as
		// internal format); without it the BGRA data is uploaded as RGBA8 and red/blue are
		// swapped by texture swizzle (core in GLES 3.0), so no CPU copy is needed.
		if (glTextureBytes == 1)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_RED;
			texformat = GL_R8;
		}
		else if (noData || noGLSwizzle)
		{
			// Without data the texture is a copy or render target (wipe screens, camera
			// textures): RGBA8 like the frame buffers it is copied from. A copy into a BGRA_EXT
			// texture is not a valid GLES 3.0 copy (GL_INVALID_OPERATION, the wipes came out empty).
			// (noGLSwizzle: the data is already in RGBA order, e.g. Selaco's lightmaps)
			texformat = GL_RGBA8;
			sourcetype = GL_RGBA;
		}
		else if (gl.bgra)
		{
			texformat = sourcetype = GL_BGRA;
		}
		else
		{
			texformat = GL_RGBA8;
			sourcetype = GL_RGBA;
			swapRB = true;
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swapRB ? GL_BLUE : GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swapRB ? GL_RED : GL_BLUE);
	}

	// Forced ETC2: the textures with data (not the copies and render targets, the 1 byte ones, Selaco's lightmaps)
	int etc2Levels = 0;
	if (firstCall && glBufferID == 0 && !noData && !noGLSwizzle && (glTextureBytes == 0 || glTextureBytes == 4))
		etc2Levels = UploadForcedETC2(buffer, rw, rh, mipmap && TexFilter[gl_texture_filter].mipmapping && !forcenofilter);

	if (etc2Levels > 0)
		;
	else if (!firstCall && glBufferID > 0)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, sourcetype, GL_UNSIGNED_BYTE, buffer);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);

	{
		sTextureUploads++;
		size_t bytes = size_t(rw) * size_t(rh) * size_t(glTextureBytes >= 1 && glTextureBytes <= 4 ? glTextureBytes : 4);
		if (mipmap && TexFilter[gl_texture_filter].mipmapping) bytes += bytes / 3;
		sTextureBytes = sTextureBytes - mTrackedBytes + bytes;
		mTrackedBytes = bytes;
	}

	if (deletebuffer && buffer) free(buffer);
	else if (glBufferID)
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}

	if (etc2Levels > 0)
	{
		mipmapped = true;	// (the levels it has; never glGenerateMipmap on a compressed texture)
	}
	else if (mipmap && TexFilter[gl_texture_filter].mipmapping && !forcenofilter)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
		mipmapped = true;
	}
	else if (!mipmap)
	{
		// We don't want mipmaps, so disable mipmaps
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);

	SetHardwareState(HardwareState::READY, texunit);

	return glTexID;
}

unsigned int FHardwareTexture::CreateCompressedTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, CompressionFormat format, const char* name)
{
	// [NL]: I know this is undercooked, but I've just put in what I need to get compressed lightmaps working.
	int dataSize = 0;
	int totalSize = 0;
	int glFormat = 0;
	int mipmaps = 1; // TODO: Implement mipmaps

	switch (format)
	{
		case IHardwareTexture::COMPRESSION_NONE:
			dataSize = w * h * 4;
			totalSize = dataSize; // TODO: Should include the size of the mipmaps
			glFormat = GL_RGBA8; // TODO: Should be smarter, I guess?
			break;

		case IHardwareTexture::COMPRESSION_BC7_UNORM:
			dataSize = (w / 4) * (h / 4) * sizeof(uint64_t) * 2;
			totalSize = dataSize; // TODO: Should include the size of the mipmaps
			glFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
			break;
		
		default:
			I_FatalError("Unsupported compression format in OpenGL: %d", format);
			return 0;
	}
	
	unsigned int result = CreateCompressedTexture(buffer, dataSize, totalSize, w, h, glFormat, texunit, mipmaps, name, !mipmap);
	SetHardwareState(HardwareState::READY, texunit);

	return result;
}

bool FHardwareTexture::SwapToLoadedImage() {
	if (glInfo.glTexID > 0) {
		assert(glTexID == 0 && glBufferID == 0);

		glTexID = glInfo.glTexID;
		sLiveTextures++;	// (the destructor counts it down again)
		glTextureBytes = glInfo.glTextureBytes;
		mipmapped = glInfo.mipmapped;
		forcenofilter = glInfo.forcenofilter;

		glInfo = GLLoadInfo();	// Reset
		
		return true;
	}

	return false;
}

// Remove background loaded image, probably because it finished loading too late
void FHardwareTexture::DestroyLoadedImage() {
	if (glInfo.glTexID > 0 && glInfo.glTexID != glTexID) {
		glDeleteTextures(1, &glInfo.glTexID);
	}
	glInfo = GLLoadInfo();
}

void FHardwareTexture::CreateInvalid(int texUnit) {
	unsigned char buff[4] = {255, 0, 255, 255};
	if (glInfo.glTexID >= 0) DestroyLoadedImage();
	CreateTexture(buff, 1, 1, texUnit, false, "FailedTexture");
}


static inline uint32_t GetFormatBlockSize(int format) {
	// 8 byte blocks: BC1 (DXT1) and its ETC2 transcode (RGB8); 16: BC3, BC7, ETC2 RGBA8 EAC
	return format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT ||
		format == BCT_RGBA_S3TC_DXT1 || format == BCT_SRGB_ALPHA_S3TC_DXT1 || format == BCT_RGB8_ETC2 || format == BCT_SRGB8_ETC2 ? 8 : 16;
}


//===========================================================================
//
// The BC compressed images (DDS: BC1, BC3, BC7) on GPUs without their format (OpenGL ES: S3TC and BPTC are
// rare there - Selaco's textures are BC7): transcoded in place to the ETC2 format with the same block size
// (etcpak, bctranscode.cpp), the size and mip layout stay. A transcode is made once: it is kept in RAM and on
// disk (etc2cache.cpp) and taken from there next time. Returns the format to upload the buffer in.
//
//===========================================================================

//===========================================================================
//
// Forced ETC2 (OpenGL ES: setForceETC2Textures, on by default; desktop GL: gl_texture_etc2_transcode 1): a
// texture with data uploaded as ETC2 RGBA8 EAC, a quarter of RGBA8's memory, its mip levels built on the CPU
// (glGenerateMipmap takes no compressed textures); made once, then from the ETC2 cache. The texture is bound.
// Returns the levels uploaded, 0 when not forced.
//
//===========================================================================

static int UploadForcedETC2(const unsigned char* bgra, int w, int h, bool mipmaps)
{
	if (!BCT_Available() || !gl.texetc2 || bgra == nullptr || w <= 0 || h <= 0 ||
		!((gl.esversion > 0 && ETC2Cache::ForceAll()) || gl_texture_etc2_transcode == 1))
		return 0;
	const int levels = mipmaps ? BCT_FullMipLevels(w, h) : 1;
	std::vector<uint8_t> etc2;
	ETC2Cache::EncodeBGRA(bgra, w, h, levels, etc2);

	size_t pos = 0;
	int lw = w, lh = h;
	for (int level = 0; level < levels; level++)
	{
		const size_t size = (size_t)std::max(1, (lw + 3) / 4) * std::max(1, (lh + 3) / 4) * 16;
		glCompressedTexImage2D(GL_TEXTURE_2D, level, BCT_RGBA8_ETC2_EAC, lw, lh, 0, (GLsizei)size, etc2.data() + pos);
		pos += size;
		lw = std::max(1, lw >> 1);
		lh = std::max(1, lh >> 1);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
	if (gl.esversion > 0)
	{
		// the ETC2 data is in RGBA order: no red / blue swizzle
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
	}
	return levels;
}

int PrepareCompressedUpload(unsigned char* buffer, size_t totalSize, int w, int h, int numMips, int format)
{
	const int kind = BCT_Kind(format);
	if (kind == 0 || !BCT_Available() || !gl.texetc2 || buffer == nullptr || totalSize == 0)
		return format;
	const bool supported = kind == 7 ? gl.texbptc : (gl.texs3tc && (!BCT_IsSRGB(format) || gl.texs3tcsrgb));
	if (supported && gl_texture_etc2_transcode == 0)
		return format;	// (the GPU takes the BC format: kept)

	const bool cached = ETC2Cache::Enabled();
	uint64_t hash = 0;
	if (cached)
	{
		hash = ETC2Cache::Hash(buffer, totalSize, w, h, format, numMips);
		if (ETC2Cache::Get(hash, buffer, totalSize))
			return BCT_ETC2Format(format);
	}
	if (BCT_TranscodeToETC2(buffer, totalSize, w, h, numMips, format) <= 0)
		return format;
	if (cached)
		ETC2Cache::Put(hash, buffer, totalSize);
	return BCT_ETC2Format(format);
}

// Add a BC7 compressed texture and mipmaps
unsigned int FHardwareTexture::BackgroundCreateCompressedTexture(GLsync* fence, unsigned char* buffer, uint32_t dataSize, uint32_t totalSize, int w, int h, int format, int texunit, int numMips, const char* name, bool forceNoMips, bool allowQualityReduction) {
	int rh, rw;
	format = PrepareCompressedUpload(buffer, totalSize, w, h, numMips, format);
	uint32_t blockSize = GetFormatBlockSize(format);

	glGetError();

	bool firstCall = glInfo.glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glInfo.glTexID);
	}
	auto err = glGetError();
	assert(err == GL_NO_ERROR);
	assert(firstCall);
	assert(texunit >= 0);

	if (texunit > 0) glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, glInfo.glTexID);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	FGLDebug::LabelObject(GL_TEXTURE, glInfo.glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (rw < w || rh < h) {
		if (texunit > 0) glActiveTexture(GL_TEXTURE0);
		return 0;	// We can't resize the texture, 
	}

	bool mipmap = numMips > 1 && !forceNoMips;

	uint32_t mipWidth = w, mipHeight = h;
	size_t mipSize = dataSize, dataPos = 0;
	const int startMip = allowQualityReduction ? min((int)gl_texture_quality, (int)numMips - 1) : 0;
	int mipCnt = 0, maxMips = mipmap ? numMips - startMip : 1;

	for (uint32_t x = 0; x < numMips && mipCnt < maxMips; x++) {
		if (mipSize == 0 || totalSize - dataPos < mipSize)
			break;

		if (x >= startMip) {
			mipCnt++;
			if (x == startMip) {
				// Base texture
				glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, mipWidth, mipHeight, 0, mipSize, buffer + dataPos);
			}
			else {
				glInfo.mipmapped = true;
				glInfo.forcenofilter = false;

				// Mip
				CreateCompressedMipmap(glInfo.glTexID, buffer + dataPos, x - startMip, mipWidth, mipHeight, format, mipSize, texunit);
			}
		}

		dataPos += mipSize;

		mipWidth = std::max(1u, (mipWidth >> 1));
		mipHeight = std::max(1u, (mipHeight >> 1));
		mipSize = std::max(1u, ((mipWidth + 3) / 4)) * std::max(1u, ((mipHeight + 3) / 4)) * blockSize;
	}

	if (forceNoMips) {
		glInfo.forcenofilter = true;
	}

	// Set the uploaded mipmap count so the sampler gets the correct data
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipCnt - 1);

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);

	glInfo.mipmapped = true;	// Mark the texture as mipmapped regardless, so the generator is never asked to create mipmaps, which it cannot do on a compressed texture


	if (fence != nullptr) {
		*fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glFlush();
	}
	else {
		glFinish();
	}

	return glInfo.glTexID;
}


unsigned int FHardwareTexture::CreateCompressedTexture(unsigned char* buffer, uint32_t dataSize, uint32_t totalSize, int w, int h, int format, int texunit, int numMips, const char* name, bool forceNoMips, bool allowQualityReduction) {
	int rh, rw;
	format = PrepareCompressedUpload(buffer, totalSize, w, h, numMips, format);
	uint32_t blockSize = GetFormatBlockSize(format);
	
	glGetError();

	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
		if (glTexID != 0) sLiveTextures++;
	}
	auto err = glGetError();
	assert(err == GL_NO_ERROR);
	assert(firstCall);

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0 + texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	FGLDebug::LabelObject(GL_TEXTURE, glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (rw < w || rh < h) {
		if (texunit > 0) glActiveTexture(GL_TEXTURE0);
		else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
		return 0;	// We can't resize the texture, 
	}

    if(numMips == 0)
    	numMips = 1;	// Guard against accidental zero
	
	bool mipmap = numMips > 1 && !forceNoMips;

	uint32_t mipWidth = w, mipHeight = h;
	size_t mipSize = dataSize, dataPos = 0;
	const int startMip = allowQualityReduction ? min((int)gl_texture_quality, (int)numMips - 1) : 0;
	int mipCnt = 0, maxMips = mipmap ? numMips - startMip : 1;

		for (uint32_t x = 0; x < numMips && mipCnt < maxMips; x++) {
			if (mipSize == 0 || totalSize - dataPos < mipSize)
				break;

			if (x >= startMip) {
				mipCnt++;
				if (x == startMip) {
					// Base texture
					glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, mipWidth, mipHeight, 0, mipSize, buffer + dataPos);
				}
				else {
					mipmapped = true;
					forcenofilter = false;

					// Mip
					CreateCompressedMipmap(glTexID, buffer + dataPos, x - startMip, mipWidth, mipHeight, format, mipSize, texunit);
				}
			}

			dataPos += mipSize;

			mipWidth = std::max(1u, (mipWidth >> 1));
			mipHeight = std::max(1u, (mipHeight >> 1));
			mipSize = (size_t)std::max(1u, ((mipWidth + 3) / 4)) * std::max(1u, ((mipHeight + 3) / 4)) * blockSize;
		}

	if (forceNoMips) {
		forcenofilter = true;
	}

	// Set the uploaded mipmap count so the sampler gets the correct data
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipCnt - 1);

	mipmapped = true; // Mark the texture as mipmapped regardless, so the generator is never asked to create mipmaps, which it cannot do on a compressed texture

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);

	return glTexID;
}


// @Cockatrice - For now I am naively assuming that sprite textures do not ever use more than one texunit.
// I do not yet understand why we use different texture units for material layers, perhaps it's because the tex unit
// is used as a slot in the shader? 
// This code will have to be significantly refactored for thread safety if it turns out there will be more than one texture load per glTexID
unsigned int FHardwareTexture::BackgroundCreateTexture(GLsync* fence, unsigned char* buffer, int w, int h, int texunit, bool mipmap, bool indexed, const char* name, bool forceNoMips)
{
	// See todotodo.txt
	int rh, rw;
	int texformat = GL_RGBA8;
	bool deletebuffer = false;

	glGetError();
	
	bool firstCall = glInfo.glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glInfo.glTexID);
	}
	auto err = glGetError();
	assert(err == GL_NO_ERROR);
	assert(firstCall);

	if (texunit > 0) glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, glInfo.glTexID);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	FGLDebug::LabelObject(GL_TEXTURE, glInfo.glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);
	
	if (rw < w || rh < h)
	{
		// The texture is larger than what the hardware can handle so scale it down.
		unsigned char* scaledbuffer = (unsigned char*)calloc(4, rw * (rh + 1));
		if (scaledbuffer)
		{
			Resize(w, h, rw, rh, buffer, scaledbuffer);
			deletebuffer = true;
			buffer = scaledbuffer;
		}
	}



	// store the physical size.
	int sourcetype;
	if (indexed)
	{
		mipmap = false;
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		texformat = GL_R8;
		sourcetype = GL_RED;
	}
	else
	{
		sourcetype = GL_BGRA;
		if (gl.esversion > 0)
		{
			// OpenGL ES 3.0: same BGRA handling as CreateTexture
			bool swapRB = !gl.bgra;
			if (gl.bgra) texformat = GL_BGRA;
			else sourcetype = GL_RGBA;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swapRB ? GL_BLUE : GL_RED);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swapRB ? GL_RED : GL_BLUE);
		}
	}


	// Forced ETC2 (see UploadForcedETC2)
	const int etc2Levels = indexed ? 0 : UploadForcedETC2(buffer, rw, rh, mipmap && TexFilter[gl_texture_filter].mipmapping && !forceNoMips);
	if (etc2Levels == 0)
		glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);

	if (deletebuffer && buffer) free(buffer);

	if (etc2Levels > 0)
	{
		glInfo.mipmapped = true;
	}
	else if (mipmap && TexFilter[gl_texture_filter].mipmapping && !forceNoMips)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
		glInfo.mipmapped = true;
	}
	else if (forceNoMips) {
		glInfo.forcenofilter = true;
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);

	if (fence != nullptr) {
		*fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glFlush();
	}
	else {
		glFinish();
	}
	
	return glInfo.glTexID;
}


bool FHardwareTexture::CreateCompressedMipmap(unsigned int texID, unsigned char* buffer, int mipLevel, int w, int h, int format, int32_t size, int texunit) {
	int rh, rw;

	if (texID == 0)
		return false;

	// Commented out for now, the few times this is called, the texture unit and texID are already bound
	//if (texunit > 0) glActiveTexture(GL_TEXTURE0 + texunit);
	//glBindTexture(GL_TEXTURE_2D, texID);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (rw < w || rh < h)
		return false;	// We can't create a mipmap of this size, so just bail since we can't work with the pixel data to shrink it

	glCompressedTexImage2D(GL_TEXTURE_2D, mipLevel, format, w, h, 0, size, buffer);

	//if (texunit > 0) glActiveTexture(GL_TEXTURE0);

	return true;
}


//===========================================================================
//
//
//
//===========================================================================
void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	int rw = GetTexDimension(w);
	int rh = GetTexDimension(h);
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;
	if (rw == w || rh == h)
	{
		glGenBuffers(1, &glBufferID);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
		// size_t arithmetic: w*h*4 overflows int for very large canvases on 32-bit builds
		const size_t bytes = size_t(w) * size_t(h) * size_t(texelsize);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_DRAW);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#ifdef ANDROID
		size = (decltype(size))bytes;
#endif
	}
}


uint8_t *FHardwareTexture::MapBuffer()
{
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
#ifdef ANDROID
	// The whole buffer is rewritten: invalidating it lets the driver hand out fresh storage
	// instead of waiting for the GPU to finish the previous upload from it.
	return (uint8_t*)glMapBufferRange (GL_PIXEL_UNPACK_BUFFER, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
#else
	return (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
#endif
}

//===========================================================================
//
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture()
{
	DestroyLoadedImage();
	if (glTexID != 0)
	{
		glDeleteTextures(1, &glTexID);
		sLiveTextures--;
		sTextureBytes -= mTrackedBytes;
	}
	if (glBufferID != 0) glDeleteBuffers(1, &glBufferID);
}


//===========================================================================
//
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, bool needmipmap)
{
	if (glTexID != 0)
	{
		if (lastbound[texunit] == glTexID) return glTexID;
		lastbound[texunit] = glTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, glTexID);
		// Check if we need mipmaps on a texture that was created without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping && !forcenofilter)
		{
			// CreateTexture limits textures created without mipmaps to their base level: lift that again
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return glTexID;
	}
	return 0;
}

void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}


//===========================================================================
//
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer(int width, int height)
{
	// Color only. The depth/stencil buffer of a camera view comes from the render target pool
	// (FGLRenderer::BindToFrameBuffer) and goes back after the view: one per size, shared by all
	// camera textures, instead of one kept per texture (which was never freed).
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID, 0);
}


//===========================================================================
//
//	Binds a texture to the renderer
//
//===========================================================================

bool FHardwareTexture::BindOrCreate(FTexture *tex, int texunit, int clampmode, int translation, int flags, bool noMips)
{
	if (noMips) forcenofilter = true;
    bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	// Bind it to the system.
	if (!Bind(texunit, needmipmap))
	{
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;
		
		// Create this texture
		FImageSource* src = tex->GetImage();
		if (src && src->IsGPUOnly()) {
			assert(glBufferID == 0);

			int fmt = src->getGLFormat();

			// Create a reader
			FileReader reader = fileSystem.OpenFileReader(src->LumpNum(), FileSys::EReaderType::READER_SHARED, 0);
			if (!reader.isOpen()) {
				Printf(TEXTCOLOR_RED "Lump: %s cannot be read: Uninitialized reader!\n", fileSystem.GetFileFullName(src->LumpNum(), false));
				SetHardwareState(HardwareState::READY, texunit);
				return false;
			}

			// Read and upload texture
			int numMipLevels;
			size_t dataSize = 0, totalSize = 0;
			unsigned char* pixelData = nullptr;
			src->ReadCompressedPixels(&reader, &pixelData, totalSize, dataSize, numMipLevels);

			if (pixelData == nullptr || dataSize == 0) {
				Printf(TEXTCOLOR_RED "Lump: %s could not be read as a compressed texture\n", fileSystem.GetFileFullName(src->LumpNum(), false));
				if (pixelData) free(pixelData);
				CreateInvalid(texunit);
				return true;
			}

			unsigned int result = CreateCompressedTexture(pixelData, (uint32_t)dataSize, (uint32_t)totalSize, tex->GetWidth(), tex->GetHeight(), fmt, texunit, numMipLevels, "::BindOrCreate(Compressed)", noMips || forcenofilter, flags & CTF_ReduceQuality);
			free(pixelData);

			if (result == 0) CreateInvalid(texunit);	// Too large for the hardware
			SetHardwareState(HardwareState::READY, texunit);
		}
		else {
			FTextureBuffer texbuffer;

			if (!tex->isHardwareCanvas())
			{
				texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
				w = texbuffer.mWidth;
				h = texbuffer.mHeight;
			}
			else
			{
				w = tex->GetWidth();
				h = tex->GetHeight();
			}
			if (!CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreate"))
			{
				// could not create texture
				return false;
			}
		}
	}
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
