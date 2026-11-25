/*
** win32glvideo.h
**
** Code to let ZDoom draw to the screen
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2010-2016 Christoph Oelckers
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

#include "win32basevideo.h"

//==========================================================================
//
//
//
//==========================================================================

class Win32GLVideo : public Win32BaseVideo
{
public:
	Win32GLVideo();

	DFrameBuffer *CreateFrameBuffer() override;
	bool InitHardware(HWND Window, int multisample);
	void Shutdown();

	void setNULLContext();
	bool setMainContext();
	bool setAuxContext(int index);
	int createAuxContext();
	int numAuxContexts();

protected:
	HGLRC m_hRC;
	int glVersion = 0;
	int glProfile = 0;
	

	HWND InitDummy();
	void ShutdownDummy(HWND dummy);
	bool SetPixelFormat();
	bool SetupPixelFormat(int multisample);
};