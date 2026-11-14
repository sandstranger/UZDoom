/*
** gl_interface.h
**
** OpenGL system interface
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2019 Christoph Oelckers
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

#ifndef R_RENDER
#define R_RENDER

#include "m_argv.h"
struct RenderContext
{
	unsigned int flags;
	unsigned int maxuniforms;
	unsigned int maxuniformblock;
	unsigned int uniformblockalignment;
	float glslversion;
	// OpenGL ES (Android): the context the driver created and the feature level the renderer uses.
	// The renderer is held to GLES 3.0 (esversion 300) even on a 3.1/3.2 context.
	int esphysical;		// e.g. 300, 310, 320; 0 on desktop GL
	int esversion;		// 300 on GLES, 0 on desktop GL
	// Capability table (desktop GL: all true / not applicable)
	bool floatlinear;		// 32-bit float textures can be sampled with linear filtering
	bool bgra;				// BGRA uploads (EXT_texture_format_BGRA8888 on GLES; desktop: always)
	bool colorbuffer16f;	// R16F/RG16F/RGBA16F are color-renderable (EXT_color_buffer_half_float/_float)
	bool colorbuffer32f;	// R32F/RG32F/RGBA32F are color-renderable (EXT_color_buffer_float)
	int msrtt;				// multisampled render-to-texture: 0 none, 1 EXT, 2 IMG
	int msrttMaxSamples;
	void *pFramebufferTexture2DMultisample;	// glFramebufferTexture2DMultisample{EXT,IMG}
	void *pRenderbufferStorageMultisampleRTT;	// glRenderbufferStorageMultisample{EXT,IMG} (the RTT-compatible one)
	int max_texturesize;
	char * vendorstring;
	char * modelstring;
};

extern RenderContext gl;

EXTERN_FARG(glversion);

#endif
