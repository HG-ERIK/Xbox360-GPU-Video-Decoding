// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#include "GpuFrame.h"
#include <xav.h>
#include "GpuH264.h"
#include "Log.h"

DWORD g_lastPresent = 0;
HANDLE g_vblankEvent = NULL;
volatile LONG g_vblanks = 0;
DWORD g_waitMs = 0;
volatile LONG g_swapBlank = 0;
volatile LONG g_missedBlanks = 0;

namespace
{
    bool  s_locked;
    bool  s_endedEarly;
    void WINAPI OnVerticalBlank( D3DVBLANKDATA* ) { InterlockedIncrement( &g_vblanks ); if( g_vblankEvent ) SetEvent( g_vblankEvent ); }
}

bool GpuFrame::Begin( bool gpu )
{
    if( !gpu )
        return true;
    if( !GpuH264::BeginFrame() )
    {
        Sleep( 1 );
        return false;
    }
    s_locked = true;
    static bool s_first = true;
    if( s_first )
    {
        s_first = false;
        Log::Write( "GPU: first frame rendered" );
    }
    return true;
}

void GpuFrame::After( bool gpu )
{
    if( s_locked && !s_endedEarly )
        GpuH264::EndFrame();
    s_locked = s_endedEarly = false;
}

void GpuFrame::Present( IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& pp, bool gpu, bool xav )
{
    DWORD elapsed = GetTickCount() - g_lastPresent;
    if( xav )
        elapsed = 13;
    if( elapsed < 13 && !gpu )
        Sleep( 13 - elapsed );
    bool hide = !xav && !gpu && GpuH264::Available();
    if( hide )
        GpuH264::PresentBegin();
    if( xav || gpu )
    {
        static HANDLE s_vblank = NULL;
        if( !s_vblank )
        {
            s_vblank = CreateEvent( NULL, FALSE, FALSE, NULL );
            device->SetVerticalBlankCallback( OnVerticalBlank );
            g_vblankEvent = s_vblank;
        }
        static IDirect3DTexture9* s_fronts[3];
        static int s_frontNext = 0, s_frontCount = -1;
        if( s_frontCount < 0 )
        {
            s_frontCount = 3;
            for( int i = 0; i < s_frontCount; ++i )
                if( FAILED( device->CreateTexture( pp.BackBufferWidth, pp.BackBufferHeight, 1, 0,
                                                   D3DFMT_LE_X8R8G8B8, 0, &s_fronts[i], NULL ) ) )
                    s_frontCount = 0;
            Log::Write( "Own front buffers: %d", s_frontCount );
        }
        IDirect3DTexture9* front = NULL;
        if( s_frontCount > 0 )
        {
            front = s_fronts[s_frontNext];
            front->AddRef();
            s_frontNext = ( s_frontNext + 1 ) % s_frontCount;
        }
        else
            device->GetFrontBuffer( &front );
        device->Resolve( D3DRESOLVE_RENDERTARGET0, NULL, front, NULL, 0, 0, NULL, 1.0f, 0, NULL );
        if( s_locked && !s_endedEarly )
        {
            GpuH264::EndFrame();
            s_endedEarly = true;
        }
        if( xav )
        {
            XAVPostRender();
            device->InsertFence();
        }
        DWORD w0 = GetTickCount();
        WaitForSingleObject( s_vblank, 50 );
        g_waitMs += GetTickCount() - w0;
        LONG now = g_vblanks;
        if( g_swapBlank && now - g_swapBlank > 1 && now - g_swapBlank < 20 )
            g_missedBlanks += now - g_swapBlank - 1;
        g_swapBlank = now;
        device->Swap( front, NULL );
        front->Release();
        if( xav )
            XAVPreRender();
        g_lastPresent = GetTickCount();
        return;
    }
    device->Present( NULL, NULL, NULL, NULL );
    if( hide )
        GpuH264::PresentEnd();
    g_lastPresent = GetTickCount();
}
