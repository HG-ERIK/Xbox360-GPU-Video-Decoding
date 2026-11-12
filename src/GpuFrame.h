// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

namespace GpuFrame
{
    bool Begin( bool gpu );
    void Present( IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& pp, bool gpu, bool xav );
    void After( bool gpu );
}
