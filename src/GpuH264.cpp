// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#include <xtl.h>
#include <xav.h>
#include <xgraphics.h>
#include <stdio.h>
#include <string.h>
#include "GpuH264.h"
#include "Log.h"

struct XAVInitEx;
XBOXAPI HRESULT WINAPI XAVInitialize( const XAVInitEx* pInit, DWORD ThreadAffinities[XAVMaxThreadTypes] );

extern "C"
{
    int   H264InitParamStructureEx( void* params, DWORD version, DWORD size );
    void* H264DecodeCreateEx( void* params, void* lowPri );
    int   H264DecodeStart( void* decoder, int flags );
    int   H264DecodeClose( void* decoder );
    int   H264DecodeGetOutputInfo( void* decoder, void* info, DWORD size );
    int   H264DecodeGetOutputGPU( void* decoder, DWORD* frame );
    int   H264DecodeReturnOutputGPU( void* decoder, DWORD handle, DWORD fence );
    int   H264PumpDecoder( void* decoder, int flags );
    HRESULT D3DDevice_ResetLowPriorityQueues( D3DDevice* device, DWORD queues, DWORD entries, DWORD flags );
    VOID NTAPI ExRegisterTitleTerminateNotification( void* registration, BOOLEAN create );
}
long D3DDevice_InsertYieldPoint( D3DDevice* device, unsigned long a, unsigned long b );

namespace
{
    IDirect3DDevice9* g_device;
    bool              g_ok;
    void*             g_decoder;
    HANDLE            g_event;
    GpuH264::ReadFn   g_read;
    void*             g_readCtx;
    BYTE              g_params[0x10C];
    DWORD             g_savedQueues;
    long              g_drawn;
    volatile bool     g_closing;
    int               g_xav = 1;
    const int         THREADS = 6, MAX_W = 1920, MAX_H = 1080, OUT_QUEUE = 8;

    // The decoder's motor thread works inside its own lock; the output calls must hold it too. Its address
    // is fixed in this build of dash.ExtraAVCodecs and checked against the code that uses it.
    CRITICAL_SECTION* g_motorLock;
    void FindMotorLock()
    {
        __try
        {
            const DWORD* code = (const DWORD*)0x9F89CAA0;
            if( code[0] == 0x3D609FB1 && code[1] == 0x7C641B78 && code[2] == 0x386B9160 )
                g_motorLock = (CRITICAL_SECTION*)( 0x9FB09160 + 0xC4 );
        }
        __except( EXCEPTION_EXECUTE_HANDLER )
        {
            g_motorLock = NULL;
        }
        Log::Write( "GPU decoder: motor lock %s", g_motorLock ? "found" : "NOT found (other decoder build)" );
    }
    DWORD g_returns[16];
    int   g_returnCount;
    void ReturnPending()
    {
        for( int i = 0; i < g_returnCount; ++i )
            H264DecodeReturnOutputGPU( g_decoder, g_returns[i], 0 );
        g_returnCount = 0;
    }
    struct MotorGuard
    {
        MotorGuard()  { if( g_motorLock ) EnterCriticalSection( g_motorLock ); }
        ~MotorGuard() { if( g_motorLock ) LeaveCriticalSection( g_motorLock ); }
    };
    struct MotorTry
    {
        bool ok;
        MotorTry() : ok( !g_motorLock || TryEnterCriticalSection( g_motorLock ) != FALSE ) {}
        ~MotorTry() { if( ok && g_motorLock ) LeaveCriticalSection( g_motorLock ); }
    };

    // Closes the decoder when the title exits, so its threads never run on freed memory.
    struct TerminateRegistration { void* routine; DWORD priority; LIST_ENTRY list; } g_terminate;
    void TerminateRoutine( TerminateRegistration* reg )
    {
        ExRegisterTitleTerminateNotification( reg, FALSE );
        if( g_decoder && g_motorLock )
        {
            bool free = false;
            for( int i = 0; i < 500 && !free; ++i )
            {
                free = TryEnterCriticalSection( g_motorLock ) != FALSE;
                if( !free )
                    Sleep( 2 );
            }
            if( !free )
                g_decoder = NULL;
            else
                LeaveCriticalSection( g_motorLock );
        }
        if( g_decoder )
        {
            g_closing = true;
            H264DecodeClose( g_decoder );
            g_decoder = NULL;
        }
    }

    int __cdecl ReadThunk( void*, BYTE** data, DWORD* length, DWORD, BYTE* info )
    {
        for( ;; )
        {
            if( g_closing )
            {
                *data = NULL;
                *length = 0;
                return 0;
            }
            GpuH264::ReadFn fn = g_read;
            if( fn )
            {
                *data = NULL;
                *length = 0;
                fn( g_readCtx, data, length, info );
                if( *length )
                    return 0;
            }
            Sleep( 2 );
        }
    }
    void __cdecl NotifyThunk( void* ev ) { SetEvent( (HANDLE)ev ); }
}

bool GpuH264::Init( IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp )
{
    FILE* ini = fopen( "game:\\gpu.ini", "r" );
    if( ini )
    {
        char line[64];
        while( fgets( line, sizeof( line ), ini ) )
            sscanf_s( line, "xav=%d", &g_xav );
        fclose( ini );
    }
    g_device = device;
    DWORD initEx[8] = { (DWORD)device, (DWORD)pp, FALSE, 1, 0, 0, 0, 0 };
    static BYTE initOut[0x80];
    initEx[7] = (DWORD)initOut;
    DWORD affinity[XAVMaxThreadTypes] = { 4, 1, 1 };
    HRESULT hr = XAVInitialize( (const XAVInitEx*)initEx, affinity );
    Log::Write( "GPU decoder: XAVInitialize %08lX", hr );
    if( FAILED( hr ) )
        return false;
    FindMotorLock();
    g_terminate.routine = (void*)TerminateRoutine;
    g_terminate.priority = 100;
    ExRegisterTitleTerminateNotification( &g_terminate, TRUE );
    // The decoder records its GPU work into low-priority command buffers; a device has no queue for them
    // until it is created here.
    hr = D3DDevice_ResetLowPriorityQueues( device, 1, 1024, 0 );
    Log::Write( "GPU decoder: low-priority queues %08lX", hr );
    g_ok = SUCCEEDED( hr );
    return g_ok;
}

bool GpuH264::Available() { return g_ok; }
bool GpuH264::XavWanted() { return g_xav != 0; }

DWORD GpuH264::DeviceThreadFlag() { return D3DCREATE_CREATE_THREAD_ON_1 | D3DCREATE_BUFFER_2_FRAMES; }

bool GpuH264::Open( ReadFn read, void* ctx, int level )
{
    if( !g_ok )
        return false;
    g_readCtx = ctx;
    g_read = read;
    if( g_decoder )
        return true;
    g_returnCount = 0;
    g_event = CreateEvent( NULL, TRUE, FALSE, NULL );
    H264InitParamStructureEx( g_params, 0x47, sizeof( g_params ) );
#define FIELD( off ) ( *(DWORD*)( g_params + ( off ) ) )
    g_params[0xE] = 1;
    FIELD( 0x10 ) = 1;
    FIELD( 0x14 ) = (DWORD)ReadThunk;  // read callback
    FIELD( 0x18 ) = 0;
    FIELD( 0x1C ) = (DWORD)NotifyThunk;  // output notification
    FIELD( 0x20 ) = (DWORD)g_event;
    FIELD( 0x24 ) = THREADS;  // CABAC worker threads
    FIELD( 0x48 ) = 1;
    FIELD( 0x58 ) = MAX_W;  // maximum picture size
    FIELD( 0x5C ) = MAX_H;
    FIELD( 0x68 ) = 0;
    FIELD( 0x70 ) = level < 41 ? 41 : level > 51 ? 51 : level;  // H.264 level
    FIELD( 0x80 ) = (DWORD)g_device;  // the app's Direct3D device
    FIELD( 0x94 ) = OUT_QUEUE;  // output pictures
    FIELD( 0x9C ) = 1;
    FIELD( 0x100 ) = 1;
    FIELD( 0x104 ) = 1;
#undef FIELD
    g_decoder = H264DecodeCreateEx( g_params, NULL );
    Log::Write( "GPU decoder: created %p", g_decoder );
    if( !g_decoder )
        return false;
    H264DecodeStart( g_decoder, 0 );
    return true;
}

void GpuH264::Detach() { g_read = NULL; }

void GpuH264::Pump()
{
    if( !g_decoder )
        return;
    MotorTry lock;
    if( lock.ok )
        ReturnPending();
}

bool GpuH264::Take( Frame& f )
{
    if( !g_decoder )
        return false;
    __declspec( align( 8 ) ) BYTE info[0xA0] = { 0 };
    MotorTry lock;
    if( !lock.ok )
        return false;
    ReturnPending();
    ResetEvent( g_event );
    if( H264DecodeGetOutputInfo( g_decoder, info, sizeof( info ) ) != 0 )
        return false;
    DWORD fr[15] = { 0 };
    if( H264DecodeGetOutputGPU( g_decoder, fr ) != 0 )
        return false;
    f.pitchY = fr[4];  f.heightY = fr[1];
    f.pitchC = fr[5];  f.heightC = fr[3];
    f.y = fr[6];  f.u = fr[7];  f.v = fr[8];
    f.cropX = fr[9];  f.cropY = fr[10];
    f.showW = fr[11]; f.showH = fr[12];
    f.handle = fr[14];
    f.stamp = *(const __int64*)( info + 0x60 );
    static bool logged = false;
    if( !logged )
    {
        logged = true;
        Log::Write( "GPU decoder: frame %lux%lu (pitch %lu, chroma %lux%lu pitch %lu), shown %lux%lu", fr[0], fr[1], fr[4], fr[2], fr[3], fr[5], fr[11], fr[12] );
    }
    return true;
}

void GpuH264::Release( const Frame& f )
{
    if( !g_decoder )
        return;
    if( g_returnCount < 16 )
        g_returns[g_returnCount++] = f.handle;
    else
    {
        MotorGuard lock;
        ReturnPending();
        H264DecodeReturnOutputGPU( g_decoder, f.handle, 0 );
    }
}

bool GpuH264::BeginFrame() { return g_ok; }

void GpuH264::EndFrame()
{
    ++g_drawn;
    g_device->InsertFence();
}

void GpuH264::PresentBegin()
{
    DWORD* q = (DWORD*)( (BYTE*)g_device + 0x6008 );
    g_savedQueues = *q;
    *q = 0;
}

void GpuH264::PresentEnd()
{
    *(DWORD*)( (BYTE*)g_device + 0x6008 ) = g_savedQueues;
}

long GpuH264::TakeDrawn()
{
    long n = g_drawn;
    g_drawn = 0;
    return n;
}

void GpuH264::Close()
{
    if( !g_decoder )
        return;
    g_read = NULL;
    {
        MotorGuard lock;
        ReturnPending();
    }
    g_closing = true;
    DWORD start = GetTickCount();
    long waiting = 0, most = 0, before = 0;
    Backlog( before, most );
    for( ;; )
    {
        Backlog( waiting, most );
        if( waiting <= 0 || GetTickCount() - start > 3000 )
            break;
        for( int i = 0; i < 16; ++i )
            D3DDevice_InsertYieldPoint( g_device, 64, 0 );
        DWORD fence = g_device->InsertFence();
        DWORD wait = GetTickCount();
        while( g_device->IsFencePending( fence ) && GetTickCount() - wait < 50 )
            Sleep( 1 );
    }
    Log::Write( "GPU decoder: queue %ld -> %ld jobs in %lu ms", before, waiting, GetTickCount() - start );
    int r = H264DecodeClose( g_decoder );
    Log::Write( "GPU decoder: closed (%d) in %lu ms", r, GetTickCount() - start );
    g_decoder = NULL;
    g_closing = false;
    if( g_event )
    {
        CloseHandle( g_event );
        g_event = NULL;
    }
}

bool GpuH264::Closing() { return g_closing; }

void GpuH264::Backlog( long& waiting, long& most )
{
    waiting = most = -1;
    if( g_decoder && *(DWORD*)g_decoder == 0x20000420 )
    {
        waiting = *(long*)( (BYTE*)g_decoder + 0x63E4 );
        most = *(long*)( (BYTE*)g_decoder + 0x63E8 );
    }
}
