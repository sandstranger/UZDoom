/*
** gles_hwtexture.cpp
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

#include "gles_system.h"

#include "c_cvars.h"
#include "hw_material.h"

#include "hw_cvars.h"
#include "gles_renderer.h"
#include "gles_samplers.h"
#include "gles_renderstate.h"
#include "gles_hwtexture.h"
#include "printf.h"
#include "bctranscode.h"
#include "etc2cache.h"
#include "filesystem.h"
#include "image.h"
#include <algorithm>

EXTERN_CVAR(Int, gl_texture_etc2_transcode)

#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D	// (GLES 3.0 / desktop GL; the compressed path runs on those only)
#endif

namespace OpenGLESRenderer
{


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
	int texformat = GL_RGBA;// TexFormat[gl_texture_format];
	bool deletebuffer=false;
	bool noData = false;

	// When running in SW mode buffer will be null, so set it to the texBuffer already created
	// There could be other use cases I do not know about which means this is a bad idea..
	if (buffer == nullptr)
		buffer = texBuffer;

	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);


	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (!buffer)
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
	bool swapRB = false;
	unsigned char *swapped = nullptr;

	if (gles.esVersion > 0 && gles.glesMode != GLES_MODE_GLES)
	{
		// OpenGL ES 2.0 / 3.0 (Android). 1 byte: GL_ALPHA (the shaders read alpha; core in both,
		// unlike GL_RED in GLES 2.0 and texture swizzles). 4 bytes arrive in BGRA order: GL_BGRA
		// needs EXT_texture_format_BGRA8888 (internal format BGRA as well); without it the data
		// is uploaded as RGBA and red/blue are swapped by texture swizzle (GLES 3.0) or on the CPU.
		if (glTextureBytes == 1)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = texformat = GL_ALPHA;
		}
		else if (noGLSwizzle)
		{
			// the data is already in RGBA order (Selaco's lightmaps)
			sourcetype = texformat = GL_RGBA;
		}
		else if (gles.bgraTextures)
		{
			sourcetype = texformat = GL_BGRA;
		}
		else
		{
			sourcetype = texformat = GL_RGBA;
			swapRB = true;
			if (!gles.textureSwizzle && buffer)
			{
				const size_t n = size_t(rw) * size_t(rh);
				swapped = (unsigned char *)malloc(n * 4);
				if (swapped)
				{
					for (size_t i = 0; i < n; i++)
					{
						swapped[i * 4 + 0] = buffer[i * 4 + 2];
						swapped[i * 4 + 1] = buffer[i * 4 + 1];
						swapped[i * 4 + 2] = buffer[i * 4 + 0];
						swapped[i * 4 + 3] = buffer[i * 4 + 3];
					}
				}
			}
		}
	}
	else if (gles.glesMode == GLES_MODE_GLES)
	{
		if (glTextureBytes == 1)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_ALPHA;
			texformat = GL_ALPHA;
		}
		else
		{
			sourcetype = GL_BGRA; // These two must be the same
			texformat = GL_BGRA;
		}
	}
	else
	{
		if (glTextureBytes == 1) //Use Red channel instread becuase Alpha does not work in OpenGL, swizzle later
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_RED;
			texformat = GL_RED;
		}
		else
		{
			sourcetype = GL_BGRA;
#ifdef ANDROID //karin: These two must be the same on OpenGLES
			texformat = GL_BGRA;
#else
			texformat = GL_RGBA;
#endif
		}
	}

	// Forced ETC2 on the GLES 3.0 path (setForceETC2Textures, on by default; desktop GL: gl_texture_etc2_transcode 1):
	// the textures with data (not the copies and render targets, the 1 byte ones, Selaco's lightmaps) as ETC2 RGBA8
	// EAC, a quarter of the memory, with mip levels built on the CPU (glGenerateMipmap takes no compressed textures)
	int etc2Levels = 0;
	if (gles.etc2Textures && gles.texMaxLevel && !noData && !noGLSwizzle && glTextureBytes == 4 &&
		((gles.esVersion > 0 && ETC2Cache::ForceAll()) || gl_texture_etc2_transcode == 1))
	{
		etc2Levels = mipmap && TexFilter[gl_texture_filter].mipmapping ? BCT_FullMipLevels(rw, rh) : 1;
		std::vector<uint8_t> etc2;
		ETC2Cache::EncodeBGRA(buffer, rw, rh, etc2Levels, etc2);
		size_t pos = 0;
		int lw = rw, lh = rh;
		for (int level = 0; level < etc2Levels; level++)
		{
			const size_t size = (size_t)std::max(1, (lw + 3) / 4) * std::max(1, (lh + 3) / 4) * 16;
			glCompressedTexImage2D(GL_TEXTURE_2D, level, BCT_RGBA8_ETC2_EAC, lw, lh, 0, (GLsizei)size, etc2.data() + pos);
			pos += size;
			lw = std::max(1, lw >> 1);
			lh = std::max(1, lh >> 1);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, etc2Levels - 1);
		swapRB = false;		// (RGBA order: no swizzle)
	}
	else
		glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, swapped ? swapped : buffer);
	if (swapped) free(swapped);

	if (gles.esVersion > 0 && gles.textureSwizzle)
	{
		// set every time: the texture object may have held another format before
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swapRB ? GL_BLUE : GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swapRB ? GL_RED : GL_BLUE);
	}

	if (gles.esVersion == 0 && gles.glesMode != GLES_MODE_GLES)
	{
		// The shader is using the alpha channel instead of red, this work on GLES but not on GL
		// So the texture uses GL_RED and this swizzels the red channel into the alpha channel
		if (glTextureBytes == 1)
		{
			GLint swizzleMask[] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
#ifdef ANDROID //karin: no GL_TEXTURE_SWIZZLE_RGBA on OpenGLES
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swizzleMask[0]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, swizzleMask[1]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swizzleMask[2]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, swizzleMask[3]);
#else
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);
#endif
		}
#ifdef _GLESxxx //karin: swap R and B as GL_BGRA on OpenGLES
		else
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
		}
#endif
	}

	if (deletebuffer && buffer) free(buffer);

	if (etc2Levels > 0)
	{
		mipmapped = true;	// (the levels it has; never glGenerateMipmap on a compressed texture)
	}
	else if (mipmap && TexFilter[gl_texture_filter].mipmapping)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
		mipmapped = true;
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
	return glTexID;
}


unsigned int FHardwareTexture::CreateCompressedTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, CompressionFormat format, const char* name)
{
	// (Selaco: compressed lightmaps) Not supported by this backend, which renders no lightmaps:
	// not fatal, so that a level with such data still loads.
	static bool warned = false;
	if (!warned)
	{
		Printf(TEXTCOLOR_ORANGE "FHardwareTexture::CreateCompressedTexture is not currently supported in OpenGLES!\n");
		warned = true;
	}
	return 0;
}

void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;

	if (texBuffer)
		delete[] texBuffer;

	texBuffer = new uint8_t[size_t(w) * size_t(h) * size_t(texelsize)];
	return;
}

uint8_t* FHardwareTexture::MapBuffer()
{
	return texBuffer;
}

//===========================================================================
//
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture()
{
	if (glTexID != 0) glDeleteTextures(1, &glTexID);

	if (texBuffer)
		delete[] texBuffer;
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
		// Check if we need mipmaps on a texture that was creted without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping)
		{
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

//===========================================================================
//
// A GPU only image (DDS: BC1 / BC3 / BC7 - Selaco's textures): uploaded as it is where the GPU has its format;
// else, on the GLES 3.0 path, transcoded to ETC2 (etcpak; made once, then taken from the ETC2 cache). Returns
// false for the rest (the GLES 2.0 path without the format): they go through the CPU, decoded to RGBA.
//
//===========================================================================

bool FHardwareTexture::CreateCompressed(FImageSource *src, int texunit)
{
	const int bcFormat = src->getGLFormat();
	const int kind = BCT_Kind(bcFormat);
	const int w = src->GetWidth(), h = src->GetHeight();
	if (kind == 0 || w <= 0 || h <= 0 || w > gles.max_texturesize || h > gles.max_texturesize || gl_texture_etc2_transcode == 2)
		return false;

	bool native = gl_texture_etc2_transcode == 0 &&
		(kind == 7 ? gles.bptcTextures : (gles.s3tcTextures && (!BCT_IsSRGB(bcFormat) || gles.s3tcSRGBTextures)));
	const bool etc2 = !native && gles.etc2Textures;
	if (!native && !etc2)
		return false;

	FileReader reader = fileSystem.OpenFileReader(src->LumpNum(), FileSys::EReaderType::READER_SHARED, 0);
	if (!reader.isOpen())
		return false;
	unsigned char *data = nullptr;
	size_t totalSize = 0, level0Size = 0;
	int mips = 0;
	src->ReadCompressedPixels(&reader, &data, totalSize, level0Size, mips);
	if (data == nullptr || totalSize == 0)
	{
		free(data);
		return false;
	}

	const size_t blockBytes = kind == 1 ? 8 : 16;
	if (!gles.texMaxLevel)
	{
		// GLES 2.0 path: no GL_TEXTURE_MAX_LEVEL - the levels must go down to 1x1 (or be decoded and mipmapped)
		int fullLevels = 1, needed = 0, lw = w, lh = h;
		while (lw > 1 || lh > 1) { lw = std::max(1, lw >> 1); lh = std::max(1, lh >> 1); fullLevels++; }
		size_t chain = 0;
		lw = w, lh = h;
		for (int level = 0; level < fullLevels; level++, needed++)
		{
			chain += (size_t)std::max(1, (lw + 3) / 4) * std::max(1, (lh + 3) / 4) * blockBytes;
			lw = std::max(1, lw >> 1); lh = std::max(1, lh >> 1);
		}
		const bool pow2 = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
		if (mips < fullLevels || totalSize < chain || (!pow2 && !gles.npotAvailable))
		{
			free(data);
			return false;
		}
	}

	int uploadFormat = bcFormat;
	if (!native)
	{
		const bool cached = ETC2Cache::Enabled();
		uint64_t hash = 0;
		bool ok = false;
		if (cached)
		{
			hash = ETC2Cache::Hash(data, totalSize, w, h, bcFormat, mips);
			ok = ETC2Cache::Get(hash, data, totalSize);
		}
		if (!ok)
		{
			ok = BCT_TranscodeToETC2(data, totalSize, w, h, mips, bcFormat) > 0;
			if (ok && cached)
				ETC2Cache::Put(hash, data, totalSize);
		}
		if (!ok)
		{
			free(data);
			return false;
		}
		uploadFormat = BCT_ETC2Format(bcFormat);
	}

	if (glTexID == 0)
		glGenTextures(1, &glTexID);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0 + texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);

	size_t pos = 0;
	int levels = 0, lw = w, lh = h;
	for (int level = 0; level < std::max(mips, 1); level++)
	{
		const size_t size = (size_t)std::max(1, (lw + 3) / 4) * std::max(1, (lh + 3) / 4) * blockBytes;
		if (totalSize - pos < size)
			break;
		glCompressedTexImage2D(GL_TEXTURE_2D, level, uploadFormat, lw, lh, 0, (GLsizei)size, data + pos);
		pos += size;
		levels++;
		lw = std::max(1, lw >> 1);
		lh = std::max(1, lh >> 1);
	}
	// (the levels there are: complete for any filter; never glGenerateMipmap on a compressed texture)
	if (gles.texMaxLevel)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(levels - 1, 0));
	mipmapped = true;
	glTextureBytes = 4;

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	free(data);
	return levels > 0;
}

bool FHardwareTexture::BindOrCreate(FTexture *tex, int texunit, int clampmode, int translation, int flags)
{
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

		FTextureBuffer texbuffer;
		FImageSource *src = tex->GetImage();

		if (src && src->IsGPUOnly() && !(flags & CTF_Indexed) && !tex->isHardwareCanvas() && CreateCompressed(src, texunit))
		{
			if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
			GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
			return true;
		}

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
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
