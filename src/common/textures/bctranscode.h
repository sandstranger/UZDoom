#pragma once

#include <stddef.h>
#include <stdint.h>

// The BC compressed images (DDS: BC1, BC3, BC7) on GPUs without the BC formats (OpenGL ES: S3TC and BPTC are rare
// there): transcoded to the ETC2 format with the same block size (core in OpenGL ES 3.0) with etcpak, block for
// block - the data keeps its size and its mip layout - or decoded to BGRA for the paths that need pixels.

enum
{
	BCT_RGB_S3TC_DXT1 = 0x83F0,
	BCT_RGBA_S3TC_DXT1 = 0x83F1,
	BCT_RGBA_S3TC_DXT5 = 0x83F3,
	BCT_SRGB_S3TC_DXT1 = 0x8C4C,
	BCT_SRGB_ALPHA_S3TC_DXT1 = 0x8C4D,
	BCT_SRGB_ALPHA_S3TC_DXT5 = 0x8C4F,
	BCT_RGBA_BPTC_UNORM = 0x8E8C,
	BCT_SRGB_ALPHA_BPTC_UNORM = 0x8E8D,

	BCT_RGB8_ETC2 = 0x9274,
	BCT_SRGB8_ETC2 = 0x9275,
	BCT_RGBA8_ETC2_EAC = 0x9278,
	BCT_SRGB8_ALPHA8_ETC2_EAC = 0x9279,
};

// The kind of BC format (1 = BC1, 3 = BC3, 7 = BC7), 0 for any other format
int BCT_Kind(int glFormat);

// The S3TC formats (BC1, BC3), else BPTC (BC7)
inline bool BCT_IsS3TC(int glFormat) { int k = BCT_Kind(glFormat); return k == 1 || k == 3; }

// The sRGB variants
bool BCT_IsSRGB(int glFormat);

// The ETC2 format a BC format is transcoded to, 0 if it is not one of the BC formats
int BCT_ETC2Format(int glFormat);

// Transcodes a BC mip chain in place: level 0 is w x h, the levels follow each other (the DDS layout). Stops at a
// level the data is too short for. Returns the number of levels transcoded.
int BCT_TranscodeToETC2(uint8_t *data, size_t totalSize, int w, int h, int numMips, int glFormat);

// Forced ETC2 for the uncompressed images: the levels of a w x h image down to 1x1, the bytes of an ETC2 RGBA8 EAC
// chain, and the encoder - a BGRA image (FBitmap's layout, rows of w pixels) to ETC2 RGBA8 EAC with its mip levels
// (box filtered here: glGenerateMipmap takes no compressed textures), the levels one after the other in out.
int BCT_FullMipLevels(int w, int h);
size_t BCT_ETC2ChainSize(int w, int h, int levels);
void BCT_EncodeBGRAToETC2(const uint8_t *bgra, int w, int h, int levels, uint8_t *out);

// Decodes level 0 of a BC image to BGRA (FBitmap's layout), pitch in bytes. Returns false for another format or too
// little data.
bool BCT_DecodeToBGRA(const uint8_t *data, size_t size, int w, int h, int glFormat, uint8_t *bgra, int pitch);
