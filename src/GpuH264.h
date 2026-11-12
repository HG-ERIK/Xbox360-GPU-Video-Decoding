// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

namespace GpuH264
{
    typedef void ( *ReadFn )( void* ctx, BYTE** data, DWORD* length, BYTE* info );

    struct Frame
    {
        DWORD handle;
        DWORD pitchY, heightY, pitchC, heightC;
        DWORD y, u, v;
        DWORD cropX, cropY, showW, showH;
        __int64 stamp;
    };

    const int LEAD_MS = 1000, MAX_IN_FLIGHT = 14;
    const int QUEUE_FRAMES = 4;
    const int RENDER_CORE = 5;

    bool Init( IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp );
    bool Available();
    bool XavWanted();
    DWORD DeviceThreadFlag();

    bool Open( ReadFn read, void* ctx, int level );
    void Detach();
    void Close();
    bool Closing();

    void Pump();
    bool Take( Frame& f );
    void Release( const Frame& f );

    bool BeginFrame();
    void EndFrame();
    void PresentBegin();
    void PresentEnd();

    void Backlog( long& waiting, long& most );
    long TakeDrawn();
}
