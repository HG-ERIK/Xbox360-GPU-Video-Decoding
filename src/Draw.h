// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

namespace Draw
{
    bool Startup( IDirect3DDevice9* device, int screenWidth, int screenHeight );

    void Rect( float x0, float y0, float x1, float y1, DWORD color );

    void Image( IDirect3DTexture9* tex, float x0, float y0, float x1, float y1, DWORD tint = 0xFFFFFFFF );

    void Frame( float x0, float y0, float x1, float y1, float thickness, DWORD color );
}
