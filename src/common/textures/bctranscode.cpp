/*
** bctranscode.cpp
** The BC compressed images (DDS: BC1, BC3, BC7) for GPUs without the BC formats: transcoded to ETC2 with etcpak
** (bcdec decodes a block row, etcpak encodes it again in the same place), or decoded to BGRA.
*/

#include <string.h>
#include <algorithm>
#include <vector>
#include "bctranscode.h"
#include "ProcessRGB.hpp"
#include "bcdec.h"

int BCT_Kind(int glFormat)
{
	switch (glFormat)
	{
	case BCT_RGB_S3TC_DXT1:
	case BCT_RGBA_S3TC_DXT1:
	case BCT_SRGB_S3TC_DXT1:
	case BCT_SRGB_ALPHA_S3TC_DXT1:
		return 1;
	case BCT_RGBA_S3TC_DXT5:
	case BCT_SRGB_ALPHA_S3TC_DXT5:
		return 3;
	case BCT_RGBA_BPTC_UNORM:
	case BCT_SRGB_ALPHA_BPTC_UNORM:
		return 7;
	default:
		return 0;
	}
}

bool BCT_IsSRGB(int glFormat)
{
	return glFormat == BCT_SRGB_S3TC_DXT1 || glFormat == BCT_SRGB_ALPHA_S3TC_DXT1 || glFormat == BCT_SRGB_ALPHA_S3TC_DXT5 ||
		glFormat == BCT_SRGB_ALPHA_BPTC_UNORM;
}

int BCT_ETC2Format(int glFormat)
{
	switch (BCT_Kind(glFormat))
	{
	case 1:		// (8 byte blocks; BC1's 1 bit alpha is not kept)
		return BCT_IsSRGB(glFormat) ? BCT_SRGB8_ETC2 : BCT_RGB8_ETC2;
	case 3:
	case 7:		// (16 byte blocks)
		return BCT_IsSRGB(glFormat) ? BCT_SRGB8_ALPHA8_ETC2_EAC : BCT_RGBA8_ETC2_EAC;
	default:
		return 0;
	}
}

static inline size_t BCT_BlockBytes(int kind)
{
	return kind == 1 ? 8 : 16;
}

// One block, RGBA out at the pitch (bytes)
static inline void BCT_DecodeBlock(int kind, const uint8_t *block, uint8_t *rgba, int pitch)
{
	switch (kind)
	{
	case 1: bcdec_bc1(block, rgba, pitch); break;
	case 3: bcdec_bc3(block, rgba, pitch); break;
	default: bcdec_bc7(block, rgba, pitch); break;
	}
}

int BCT_TranscodeToETC2(uint8_t *data, size_t totalSize, int w, int h, int numMips, int glFormat)
{
	const int kind = BCT_Kind(glFormat);
	if (kind == 0 || data == nullptr || w <= 0 || h <= 0)
		return 0;

	const size_t blockBytes = BCT_BlockBytes(kind);
	std::vector<uint8_t> row;	// one row of blocks, 4 pixel rows
	size_t pos = 0;
	int levels = 0;

	for (int level = 0; level < std::max(numMips, 1); level++)
	{
		const int blocksX = std::max(1, (w + 3) / 4);
		const int blocksY = std::max(1, (h + 3) / 4);
		const size_t rowBytes = (size_t)blocksX * blockBytes;
		if (totalSize < pos || totalSize - pos < rowBytes * blocksY)
			break;

		const int rowPixels = blocksX * 4;
		row.resize((size_t)rowPixels * 4 * 4);
		for (int by = 0; by < blocksY; by++)
		{
			uint8_t *src = data + pos + (size_t)by * rowBytes;
			for (int bx = 0; bx < blocksX; bx++)
				BCT_DecodeBlock(kind, src + (size_t)bx * blockBytes, row.data() + (size_t)bx * 16, rowPixels * 4);

			// etcpak reads BGRA
			for (size_t i = 0; i < row.size(); i += 4)
				std::swap(row[i], row[i + 2]);

			// The ETC2 blocks have the size of the BC ones: written over the row just decoded
			if (kind == 1)
				CompressEtc2Rgb((const uint32_t *)row.data(), (uint64_t *)src, blocksX, rowPixels, true);
			else
				CompressEtc2Rgba((const uint32_t *)row.data(), (uint64_t *)src, blocksX, rowPixels, true);
		}

		pos += rowBytes * blocksY;
		levels++;
		w = std::max(1, w >> 1);
		h = std::max(1, h >> 1);
	}
	return levels;
}

bool BCT_DecodeToBGRA(const uint8_t *data, size_t size, int w, int h, int glFormat, uint8_t *bgra, int pitch)
{
	const int kind = BCT_Kind(glFormat);
	if (kind == 0 || data == nullptr || w <= 0 || h <= 0)
		return false;

	const size_t blockBytes = BCT_BlockBytes(kind);
	const int blocksX = std::max(1, (w + 3) / 4);
	const int blocksY = std::max(1, (h + 3) / 4);
	if (size < (size_t)blocksX * blocksY * blockBytes)
		return false;

	uint8_t block[4 * 4 * 4];
	for (int by = 0; by < blocksY; by++)
	{
		for (int bx = 0; bx < blocksX; bx++)
		{
			BCT_DecodeBlock(kind, data + ((size_t)by * blocksX + bx) * blockBytes, block, 16);
			for (int y = 0; y < 4 && by * 4 + y < h; y++)
			{
				uint8_t *out = bgra + (size_t)(by * 4 + y) * pitch + (size_t)bx * 16;
				const uint8_t *in = block + y * 16;
				for (int x = 0; x < 4 && bx * 4 + x < w; x++)
				{
					out[x * 4 + 0] = in[x * 4 + 2];
					out[x * 4 + 1] = in[x * 4 + 1];
					out[x * 4 + 2] = in[x * 4 + 0];
					out[x * 4 + 3] = in[x * 4 + 3];
				}
			}
		}
	}
	return true;
}

int BCT_FullMipLevels(int w, int h)
{
	int levels = 1;
	while (w > 1 || h > 1)
	{
		w = std::max(1, w >> 1);
		h = std::max(1, h >> 1);
		levels++;
	}
	return levels;
}

size_t BCT_ETC2ChainSize(int w, int h, int levels)
{
	size_t size = 0;
	for (int level = 0; level < levels; level++)
	{
		size += (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 16;
		w = std::max(1, w >> 1);
		h = std::max(1, h >> 1);
	}
	return size;
}

void BCT_EncodeBGRAToETC2(const uint8_t *bgra, int w, int h, int levels, uint8_t *out)
{
	std::vector<uint8_t> level(bgra, bgra + (size_t)w * h * 4), next, padded;
	for (int l = 0; l < levels; l++)
	{
		// whole blocks: the edge pixels repeated
		const int blocksX = std::max(1, (w + 3) / 4), blocksY = std::max(1, (h + 3) / 4);
		const int pw = blocksX * 4, ph = blocksY * 4;
		padded.resize((size_t)pw * ph * 4);
		for (int y = 0; y < ph; y++)
		{
			const uint8_t *src = level.data() + (size_t)std::min(y, h - 1) * w * 4;
			uint8_t *dst = padded.data() + (size_t)y * pw * 4;
			memcpy(dst, src, (size_t)w * 4);
			for (int x = w; x < pw; x++)
				memcpy(dst + (size_t)x * 4, src + (size_t)(w - 1) * 4, 4);
		}
		CompressEtc2Rgba((const uint32_t *)padded.data(), (uint64_t *)out, blocksX * blocksY, pw, true);
		out += (size_t)blocksX * blocksY * 16;

		if (l + 1 < levels)
		{
			// the next level: the average of 2x2 pixels (1x2 / 2x1 at an odd edge)
			const int nw = std::max(1, w >> 1), nh = std::max(1, h >> 1);
			next.resize((size_t)nw * nh * 4);
			for (int y = 0; y < nh; y++)
			{
				const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
				for (int x = 0; x < nw; x++)
				{
					const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
					const uint8_t *a = level.data() + ((size_t)y0 * w + x0) * 4, *b = level.data() + ((size_t)y0 * w + x1) * 4;
					const uint8_t *c = level.data() + ((size_t)y1 * w + x0) * 4, *d = level.data() + ((size_t)y1 * w + x1) * 4;
					uint8_t *o = next.data() + ((size_t)y * nw + x) * 4;
					for (int i = 0; i < 4; i++)
						o[i] = (uint8_t)((a[i] + b[i] + c[i] + d[i] + 2) >> 2);
				}
			}
			level.swap(next);
			w = nw;
			h = nh;
		}
	}
}
