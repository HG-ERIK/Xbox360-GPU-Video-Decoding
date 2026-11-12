// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#include <xtl.h>
#include <xaudio2.h>
#include <d3dx9.h>
#include <stdio.h>
#include "FFPlayer.h"
#include "ffglue.h"
#include "Http.h"
#include "Log.h"
#include "Guard.h"
#include <xgraphics.h>
#include <deque>
#include <set>
#include "GpuH264.h"
#include <xav.h>

struct FFPlayer::GpuState
{
    CRITICAL_SECTION                 lock;
    HANDLE                           dataEvent;
    std::deque< std::vector<BYTE> >  chunks;
    std::deque<__int64>              chunkStamps;
    std::multiset<__int64>           stamps;
    std::vector<BYTE>                stage;
    volatile long                    fed, taken;
    volatile LONG                    flushGen;
    LONG                             seenGen;
    double                           lastT, frameDur;
    int                              level;
    bool                             opened;
    volatile bool                    needHeaders;
    GpuH264::Frame                   q[8];
    double                           qPts[8];
    void PopFront() { for( int i = 1; i < qn; ++i ) { q[i - 1] = q[i]; qPts[i - 1] = qPts[i]; } --qn; }
    int                              qn;
    GpuH264::Frame                   cur;
    bool                             held;
    volatile double                  firstPts;
    volatile LONG                    ready;
    DWORD                            flushTick;
    long                             skipped;
    D3DTexture                       tex[3];
    double                           vClock;
    double                           vBias;
    LONG                             vBlank;
    bool                             vOn;
    DWORD                            renderThread, oldCore;
    int                              oldPriority;
    GpuState() : dataEvent( CreateEvent( NULL, FALSE, FALSE, NULL ) ), fed( 0 ), taken( 0 ), flushGen( 0 ), seenGen( 0 ),
                 lastT( 0 ), frameDur( 1.0 / 25 ), level( 41 ), opened( false ), needHeaders( true ), qn( 0 ), held( false ), firstPts( -1.0 ), ready( 0 ), flushTick( 0 ), skipped( 0 ),
                 vClock( 0 ), vBias( 0 ), vBlank( 0 ), vOn( false ), renderThread( 0 ), oldCore( 0 ), oldPriority( 0 )
    {
        InitializeCriticalSection( &lock );
    }
    ~GpuState() { CloseHandle( dataEvent ); DeleteCriticalSection( &lock ); }
};
extern volatile LONG g_missedBlanks;
static LONG s_lateFrames;

namespace
{
    const size_t MAX_VIDEO_PACKETS = 300;
    const size_t MAX_AUDIO_PACKETS = 600;
    const int    PCM_BYTES         = 192000 * 4;
    const __int64 SKIP_READ_LIMIT  = 512 * 1024;

    const DWORD DEMUX_CPU = 1;
    const DWORD VIDEO_CPU = 2;
    const DWORD AUDIO_CPU = 1;

    const char* VS_SOURCE =
        "struct VsIn  { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VsOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "VsOut main( VsIn i ) { VsOut o; o.pos = i.pos; o.uv = i.uv; return o; }\n";

    const char* PS_SOURCE =
        "sampler2D texY : register( s0 );\n"
        "sampler2D texU : register( s1 );\n"
        "sampler2D texV : register( s2 );\n"
        "float4 rowR : register( c0 );\n"
        "float4 rowG : register( c1 );\n"
        "float4 rowB : register( c2 );\n"
        "float4 levels : register( c3 );\n"
        "float4 main( float2 uv : TEXCOORD0 ) : COLOR {\n"
        "  float4 yuv = float4( tex2D( texY, uv ).r - levels.x, tex2D( texU, uv ).r - 0.5,\n"
        "                       tex2D( texV, uv ).r - 0.5, 1.0 );\n"
        "  return float4( dot( yuv, rowR ), dot( yuv, rowG ), dot( yuv, rowB ), 1.0 ) + levels.y;\n"
        "}\n";

    const float BT601[3][4]      = { { 1.164f, 0.0f, 1.596f, 0 }, { 1.164f, -0.391f, -0.813f, 0 }, { 1.164f, 2.018f, 0.0f, 0 } };
    const float BT709[3][4]      = { { 1.164f, 0.0f, 1.793f, 0 }, { 1.164f, -0.213f, -0.533f, 0 }, { 1.164f, 2.112f, 0.0f, 0 } };
    const float BT601_FULL[3][4] = { { 1.0f, 0.0f, 1.402f, 0 }, { 1.0f, -0.344f, -0.714f, 0 }, { 1.0f, 1.772f, 0.0f, 0 } };
    const float BT709_FULL[3][4] = { { 1.0f, 0.0f, 1.575f, 0 }, { 1.0f, -0.187f, -0.468f, 0 }, { 1.0f, 1.856f, 0.0f, 0 } };

    struct QuadVertex
    {
        float x, y, z, w;
        float u, v;
    };

    const D3DVERTEXELEMENT9 QUAD_ELEMENTS[] =
    {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    std::wstring FormatTime( double s )
    {
        if( s < 0 )
            s = 0;
        unsigned secs = (unsigned)s;
        wchar_t buf[32];
        if( secs >= 3600 )
            swprintf_s( buf, L"%u:%02u:%02u", secs / 3600, ( secs / 60 ) % 60, secs % 60 );
        else
            swprintf_s( buf, L"%u:%02u", secs / 60, secs % 60 );
        return buf;
    }

    std::wstring Widen( const char* s )
    {
        std::wstring w;
        for( ; *s; ++s )
            w += (wchar_t)(unsigned char)*s;
        return w;
    }

    void FFmpegLog( const char* line )
    {
        std::string s( line );
        while( !s.empty() && ( s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r' ) )
            s.erase( s.size() - 1 );
        if( !s.empty() )
            Log::Write( "ffmpeg: %s", s.c_str() );
    }

    HANDLE StartThread( LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD cpu )
    {
        HANDLE h = Guard_CreateThread( 256 * 1024, fn, arg, CREATE_SUSPENDED );
        XSetThreadProcessor( h, cpu );
        ResumeThread( h );
        return h;
    }
}

FFPlayer::FFPlayer()
    : m_device( NULL ), m_vs( NULL ), m_ps( NULL ), m_decl( NULL ), m_texW( 0 ), m_texH( 0 ),
      m_haveFrame( false ), m_frameW( 0 ), m_frameH( 0 ), m_videoW( 0 ), m_videoH( 0 ), m_decodeThreads( 0 ), m_frameColorspace( 0 ), m_frameFullRange( 0 ),
      m_brightness( 0.0f ), m_xaudio( NULL ), m_master( NULL ),
      m_voice( NULL ), m_voiceRate( 0 ), m_voiceChannels( 0 ), m_audioNext( 0 ), m_samplesAtBase( 0 ),
      m_audioBasePts( 0 ), m_audioStarted( 0 ), m_quit( 0 ), m_seekRequest( 0 ), m_seekTarget( 0 ),
      m_flushing( 0 ), m_decodersParked( 0 ), m_current( 0 ), m_stream( NULL ), m_subWanted( -1 ), m_subChanged( 0 ), m_file( INVALID_HANDLE_VALUE ), m_streamPos( 0 ),
      m_streamSize( -1 ), m_media( NULL ), m_hasAudio( false ), m_hasVideo( false ), m_lastShowTick( 0 ), m_duration( 0 ), m_startSeconds( 0 ), m_timeOffset( 0 ),
      m_active( false ), m_opened( 0 ), m_ended( 0 ), m_paused( false ), m_wallStart( 0 ), m_wallBase( 0 ),
      m_lastPts( 0 ), m_decoded( 0 ), m_dropped( 0 ), m_shown( 0 ), m_catchups( 0 ), m_bytesRead( 0 ), m_skippingB( false ), m_catchLevel( 0 ), m_seekFloor( -1.0 ), m_reachingFloor( 0 ), m_speedTest( false ),
      m_aspect( ASPECT_AUTO ), m_pixelAspect( 1.0 ), m_tvAspect( 16.0f / 9.0f ), m_screenSaverOff( false ), m_connectionLost( 0 ),
      m_statsTick( 0 )
      , m_g( NULL ), m_gpuWanted( true ), m_gpuOnly( false ), m_x( NULL ), m_psRgb( NULL )
{
    m_tex[0] = m_tex[1] = m_tex[2] = NULL;
    m_threads[0] = m_threads[1] = m_threads[2] = NULL;
    InitializeCriticalSection( &m_lock );
    m_videoData = CreateEvent( NULL, FALSE, FALSE, NULL );
    m_audioData = CreateEvent( NULL, FALSE, FALSE, NULL );
    m_room = CreateEvent( NULL, FALSE, FALSE, NULL );
    m_frameFree = CreateEvent( NULL, FALSE, FALSE, NULL );
    for( int i = 0; i < FRAME_COUNT; ++i )
        m_frames[i].ready = 0;
}

FFPlayer::~FFPlayer()
{
    Stop();
    for( int i = 0; i < 3; ++i )
        if( m_tex[i] )
            m_tex[i]->Release();
    if( m_vs )   m_vs->Release();
    if( m_ps )   m_ps->Release();
    if( m_decl ) m_decl->Release();
    if( m_master ) m_master->DestroyVoice();
    if( m_xaudio ) m_xaudio->Release();
    CloseHandle( m_videoData );
    CloseHandle( m_audioData );
    CloseHandle( m_room );
    CloseHandle( m_frameFree );
    DeleteCriticalSection( &m_lock );
}

bool FFPlayer::Startup( IDirect3DDevice9* device )
{
    if( m_device )
        return true;
    m_device = device;
    fg_init( FFmpegLog );

    if( !CreateShaders() )
        return false;

    HRESULT hr = XAudio2Create( &m_xaudio, 0, XboxThread5 );
    if( SUCCEEDED( hr ) )
        hr = m_xaudio->CreateMasteringVoice( &m_master );
    if( FAILED( hr ) )
    {
        Log::Write( "XAudio2 init failed 0x%08X - video only", hr );
        if( m_xaudio ) { m_xaudio->Release(); m_xaudio = NULL; }
        m_master = NULL;
    }
    Log::Write( "FFPlayer ready" );
    return true;
}

bool FFPlayer::CreateShaders()
{
    ID3DXBuffer* code = NULL;
    ID3DXBuffer* errors = NULL;
    HRESULT hr = D3DXCompileShader( VS_SOURCE, (UINT)strlen( VS_SOURCE ), NULL, NULL, "main", "vs_3_0",
                                    0, &code, &errors, NULL );
    if( SUCCEEDED( hr ) )
    {
        m_device->CreateVertexShader( (const DWORD*)code->GetBufferPointer(), &m_vs );
        code->Release();
        code = NULL;
        hr = D3DXCompileShader( PS_SOURCE, (UINT)strlen( PS_SOURCE ), NULL, NULL, "main", "ps_3_0",
                                0, &code, &errors, NULL );
    }
    if( SUCCEEDED( hr ) )
    {
        m_device->CreatePixelShader( (const DWORD*)code->GetBufferPointer(), &m_ps );
        code->Release();
    }
    if( FAILED( hr ) )
    {
        Log::Write( "Shader compile failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?" );
        if( errors )
            errors->Release();
        return false;
    }
    m_device->CreateVertexDeclaration( QUAD_ELEMENTS, &m_decl );
    return m_vs && m_ps && m_decl;
}

void FFPlayer::Open( const std::vector<std::string>& urls, const std::vector<std::wstring>& labels,
                     const std::string& headers, double startSeconds, double timeOffset, double durationHint )
{
    Stop();
    m_timeOffset = timeOffset;
    m_urls = urls;
    m_labels = labels;
    m_headers = headers;
    m_startSeconds = startSeconds;
    m_current = 0;
    m_error.clear();
    m_info.clear();
    m_videoW = m_videoH = 0;
    m_quit =m_seekRequest = m_flushing = m_decodersParked = 0;
    m_opened = m_ended = m_audioStarted = 0;
    m_paused = false;
    m_haveFrame = false;
    m_duration = durationHint;
    m_lastPts = startSeconds;
    m_decoded = m_dropped = m_shown = m_catchups = 0;
    m_catchLevel = 0;
    m_bytesRead = 0;
    m_skippingB = false;
    m_seekFloor = -1.0;
    m_reachingFloor = 0;
    m_connectionLost = 0;
    m_pixelAspect = 1.0;
    XVIDEO_MODE mode;
    XGetVideoMode( &mode );
    m_tvAspect = mode.fIsWideScreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    m_statsTick = GetTickCount();
    for( int i = 0; i < FRAME_COUNT; ++i )
        m_frames[i].ready = 0;
    m_tracks.clear();
    m_cues.clear();
    m_external.clear();
    m_subWanted = -1;
    m_subChanged = 0;

    m_active = true;
    if( XavOpen() )
        return;
    m_threads[0] = StartThread( DemuxThread, this, DEMUX_CPU );
}

void FFPlayer::Stop()
{
    if( !m_active )
        return;
    Log::Write( "Player stopping" );
    if( m_x )
    {
        XavStop();
        m_active = false;
        KeepScreenAwake( false );
        Log::Write( "Player stopped (XAV: shown %ld, dropped %ld)", m_shown, m_dropped );
        return;
    }
    InterlockedExchange( &m_quit, 1 );
    WakeAll();

    EnterCriticalSection( &m_lock );
    if( m_stream )
        m_stream->Abort();
    LeaveCriticalSection( &m_lock );

    static const char* const names[3] = { "demux", "video", "audio" };
    for( int i = 0; i < 3; ++i )
    {
        if( !m_threads[i] )
            continue;
        while( WaitForSingleObject( m_threads[i], 2000 ) != WAIT_OBJECT_0 )
        {
            Log::Write( "Waiting for %s thread to stop", names[i] );
            WakeAll();
        }
        CloseHandle( m_threads[i] );
        m_threads[i] = NULL;
    }
    Log::Write( "Player threads stopped" );
    if( m_voice )
    {
        m_voice->DestroyVoice();
        m_voice = NULL;
        m_voiceRate = m_voiceChannels = 0;
    }
    ClearQueue( m_videoQ );
    ClearQueue( m_audioQ );
    if( m_media )
    {
        fg_close( m_media );
        m_media = NULL;
    }
    Log::Write( "Player media closed" );
    GpuClose();
    delete m_stream;
    m_stream = NULL;
    if( m_file != INVALID_HANDLE_VALUE )
    {
        CloseHandle( m_file );
        m_file = INVALID_HANDLE_VALUE;
    }
    m_active = false;
    KeepScreenAwake( false );
    Log::Write( "Player stopped (decoded %ld, shown %ld, dropped %ld)", m_decoded, m_shown, m_dropped );
}

int FFPlayer::SourceRead( void* opaque, unsigned char* buf, int size )
{
    FFPlayer* p = (FFPlayer*)opaque;
    if( p->m_file != INVALID_HANDLE_VALUE )
    {
        DWORD got = 0;
        if( p->m_quit || !ReadFile( p->m_file, buf, size, &got, NULL ) )
            return -1;
        p->m_streamPos += got;
        p->m_bytesRead += got;
        return (int)got;
    }
    static const DWORD waits[] = { 0, 1000, 2000, 3000, 5000, 8000 };
    for( int attempt = 0; ; ++attempt )
    {
        if( p->m_quit )
            return -1;
        int n = p->m_stream->Read( (char*)buf, size );
        if( n >= 0 )
        {
            p->m_streamPos += n;
            p->m_bytesRead += n;
            return n;
        }
        for( ;; )
        {
            if( attempt >= (int)( sizeof( waits ) / sizeof( waits[0] ) ) )
            {
                Log::Write( "Stream lost at %I64d, giving up", p->m_streamPos );
                InterlockedExchange( &p->m_connectionLost, 1 );
                return -1;
            }
            Log::Write( "Stream read failed at %I64d, reconnecting (try %d)", p->m_streamPos, attempt + 1 );
            for( DWORD waited = 0; waited < waits[attempt] && !p->m_quit; waited += 100 )
                Sleep( 100 );
            if( p->m_quit )
                return -1;
            std::string err;
            if( p->m_stream->Open( p->m_urls[p->m_current], p->m_headers, p->m_streamPos, err ) )
                break;
            Log::Write( "Reconnect failed: %s", err.c_str() );
            ++attempt;
        }
    }
}

int FFPlayer::SourceSeek( void* opaque, __int64 pos )
{
    FFPlayer* p = (FFPlayer*)opaque;
    if( pos == p->m_streamPos )
        return 0;
    if( p->m_file != INVALID_HANDLE_VALUE )
    {
        LARGE_INTEGER to;
        to.QuadPart = pos;
        if( !SetFilePointerEx( p->m_file, to, NULL, FILE_BEGIN ) )
            return -1;
        p->m_streamPos = pos;
        return 0;
    }
    if( pos > p->m_streamPos && pos - p->m_streamPos < SKIP_READ_LIMIT )
    {
        char scratch[16384];
        while( p->m_streamPos < pos )
        {
            int want = (int)min( (__int64)sizeof( scratch ), pos - p->m_streamPos );
            int n = p->m_stream->Read( scratch, want );
            if( n <= 0 )
                break;
            p->m_streamPos += n;
        }
        if( p->m_streamPos == pos )
            return 0;
    }
    std::string err;
    if( !p->m_stream->Open( p->m_urls[p->m_current], p->m_headers, pos, err ) )
    {
        Log::Write( "Seek to %I64d failed: %s", pos, err.c_str() );
        return -1;
    }
    p->m_streamPos = pos;
    return 0;
}

bool FFPlayer::OpenStream( std::string& error )
{
    Http::Stream* fresh = new Http::Stream;
    EnterCriticalSection( &m_lock );
    delete m_stream;
    m_stream = fresh;
    LeaveCriticalSection( &m_lock );
    m_streamPos = 0;
    bool opened = m_stream->Open( m_urls[m_current], m_headers, 0, error );
    for( int retry = 0; !opened && retry < 8 && !m_quit && error.find( "HTTP 503" ) != std::string::npos; ++retry )
    {
        Log::Write( "Player source %u busy (503), retrying", (unsigned)m_current );
        Sleep( 750 );
        Http::Stream* again = new Http::Stream;
        EnterCriticalSection( &m_lock );
        delete m_stream;
        m_stream = again;
        LeaveCriticalSection( &m_lock );
        opened = m_stream->Open( m_urls[m_current], m_headers, 0, error );
    }
    if( !opened )
    {
        Log::Write( "Player source %u failed: %s", (unsigned)m_current, error.c_str() );
        return false;
    }
    m_streamSize = m_stream->TotalSize();
    return true;
}

bool FFPlayer::OpenMedia( std::string& error )
{
    for( m_current = 0; m_current < m_urls.size(); ++m_current )
    {
        if( m_urls[m_current].find( "://" ) == std::string::npos )
        {
            m_file = CreateFileA( m_urls[m_current].c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL );
            if( m_file == INVALID_HANDLE_VALUE )
            {
                error = "Can't open the file";
                Log::Write( "Player source %u: can't open %s", (unsigned)m_current, m_urls[m_current].c_str() );
                continue;
            }
            LARGE_INTEGER size;
            GetFileSizeEx( m_file, &size );
            m_streamSize = size.QuadPart;
            m_streamPos = 0;
        }
        else if( !OpenStream( error ) )
            continue;

        FgSource src;
        src.opaque = this;
        src.read = SourceRead;
        src.seek = SourceSeek;
        src.size = m_streamSize;

        char err[128] = "";
        DWORD start = GetTickCount();
        m_media = fg_open( &src, "stream", err, sizeof( err ) );
        if( !m_media )
        {
            error = err;
            Log::Write( "Player source %u: %s", (unsigned)m_current, err );
            continue;
        }

        FgInfo info;
        fg_get_info( m_media, &info );
        if( info.duration > 0 && m_timeOffset == 0 )
            m_duration = info.duration;
        m_hasAudio = info.hasAudio && m_xaudio;
        m_hasVideo = info.hasVideo != 0;
        if( info.pixelAspect > 0.25 && info.pixelAspect < 4.0 )
            m_pixelAspect = info.pixelAspect;
        wchar_t buf[160];
        swprintf_s( buf, L"%s %dx%d%s%s", Widen( info.videoCodec ).c_str(), info.width, info.height,
                    info.hasAudio ? L" / " : L"", Widen( info.audioCodec ).c_str() );
        m_info = buf;
        m_decodeThreads = info.threads;
        m_videoW = info.hasVideo ? info.width : 0;
        m_videoH = info.hasVideo ? info.height : 0;
        Log::Write( "Opened %s: %s %dx%d %.2f fps, audio %s %d Hz %d ch, %.0f s (%u ms); %d threads (%s), profile %d level %d refs %d",
                    info.container, info.videoCodec, info.width, info.height, info.fps, info.audioCodec,
                    info.sampleRate, info.channels, info.duration, GetTickCount() - start, info.threads,
                    info.frameThreading == 1 ? "frame" : info.frameThreading == 2 ? "slice" : "NONE",
                    info.profile, info.level, info.refs );

        Log::Write( "VMX chroma: %s (%d), luma: %s (%d)", fg_vmx_status() == 0 ? "in use" : "not used",
                    fg_vmx_status(), fg_vmx_qpel_status() == 0 ? "in use" : "not used", fg_vmx_qpel_status() );

        GpuMaybeStart( info );
        if( m_gpuOnly && m_hasVideo && !m_g )
        {
            error = "Not supported: the GPU plays H.264 (8-bit) up to 1080p, and WMV";
            Log::Write( "GPU only: %s %dx%d profile %d refused", info.videoCodec, info.width, info.height, info.profile );
            return false;
        }
        fg_set_fast( m_media, 0 );
        return true;
    }
    return false;
}

void FFPlayer::WakeAll()
{
    SetEvent( m_videoData );
    SetEvent( m_audioData );
    SetEvent( m_room );
    SetEvent( m_frameFree );
}

void FFPlayer::Push( PacketQueue& q, HANDLE dataEvent, FgPacket* p )
{
    EnterCriticalSection( &m_lock );
    q.items.push_back( p );
    LeaveCriticalSection( &m_lock );
    SetEvent( dataEvent );
}

FgPacket* FFPlayer::Pop( PacketQueue& q, HANDLE dataEvent, bool& eof )
{
    for( ;; )
    {
        if( m_quit )
            return NULL;
        if( m_flushing )
        {
            InterlockedIncrement( &m_decodersParked );
            while( m_flushing && !m_quit )
                Sleep( 2 );
            InterlockedDecrement( &m_decodersParked );
            eof = false;
            return NULL;
        }
        EnterCriticalSection( &m_lock );
        FgPacket* p = NULL;
        if( !q.items.empty() )
        {
            p = q.items.front();
            q.items.pop_front();
        }
        eof = q.items.empty() && q.eof;
        LeaveCriticalSection( &m_lock );
        if( p || eof )
        {
            SetEvent( m_room );
            return p;
        }
        WaitForSingleObject( dataEvent, 20 );
    }
}

void FFPlayer::ClearQueue( PacketQueue& q )
{
    EnterCriticalSection( &m_lock );
    for( size_t i = 0; i < q.items.size(); ++i )
        fg_free_packet( q.items[i] );
    q.items.clear();
    q.eof = false;
    LeaveCriticalSection( &m_lock );
}

DWORD WINAPI FFPlayer::DemuxThread( LPVOID p ) { ( (FFPlayer*)p )->DemuxLoop(); return 0; }
DWORD WINAPI FFPlayer::VideoThread( LPVOID p ) { ( (FFPlayer*)p )->VideoLoop(); return 0; }
DWORD WINAPI FFPlayer::AudioThread( LPVOID p ) { ( (FFPlayer*)p )->AudioLoop(); return 0; }

void FFPlayer::DoSeek( double seconds )
{
    int decoders = m_hasAudio ? 2 : 1;
    InterlockedExchange( &m_flushing, 1 );
    WakeAll();
    while( m_decodersParked < decoders && !m_quit )
        Sleep( 2 );

    if( m_g )
        GpuFlush();
    ClearQueue( m_videoQ );
    ClearQueue( m_audioQ );
    EnterCriticalSection( &m_lock );
    for( int i = 0; i < FRAME_COUNT; ++i )
        m_frames[i].ready = 0;
    LeaveCriticalSection( &m_lock );
    if( m_voice )
    {
        m_voice->Stop( 0 );
        m_voice->FlushSourceBuffers();
    }
    m_audioStarted = 0;
    m_ended = 0;

    if( fg_seek( m_media, seconds ) != 0 )
        Log::Write( "Seek to %.1f s failed", seconds );
    else
        Log::Write( "Seek to %.1f s", seconds );
    m_lastPts = seconds;
    m_wallBase = seconds;
    m_wallStart = GetTickCount();
    m_seekFloor = seconds;
    m_reachingFloor = m_hasVideo ? 1 : 0;

    InterlockedExchange( &m_seekRequest, 0 );
    InterlockedExchange( &m_flushing, 0 );
}

void FFPlayer::DemuxLoop()
{
    std::string err;
    if( !OpenMedia( err ) )
    {
        m_error = err.empty() ? "Could not open the video" : err;
        InterlockedExchange( &m_ended, 1 );
        return;
    }

    m_wallBase = 0;
    m_wallStart = GetTickCount();
    m_threads[1] = StartThread( VideoThread, this, VIDEO_CPU );
    if( m_hasAudio )
        m_threads[2] = StartThread( AudioThread, this, AUDIO_CPU );
    std::vector<FgTrack> tracks;
    for( int i = 0; i < fg_track_count( m_media ); ++i )
    {
        FgTrack tr;
        fg_get_track( m_media, i, &tr );
        tracks.push_back( tr );
    }
    EnterCriticalSection( &m_lock );
    m_tracks = tracks;
    LeaveCriticalSection( &m_lock );
    InterlockedExchange( &m_opened, 1 );

    if( m_startSeconds > 1.0 )
        DoSeek( m_startSeconds );

    bool eof = false;
    while( !m_quit )
    {
        if( InterlockedExchange( &m_subChanged, 0 ) )
        {
            fg_select_subtitle( m_media, m_subWanted );
            EnterCriticalSection( &m_lock );
            m_cues.clear();
            LeaveCriticalSection( &m_lock );
        }
        if( m_seekRequest )
        {
            DoSeek( m_seekTarget );
            eof = false;
            continue;
        }
        if( eof )
        {
            WaitForSingleObject( m_room, 50 );
            continue;
        }

        EnterCriticalSection( &m_lock );
        bool full = m_videoQ.items.size() >= MAX_VIDEO_PACKETS || m_audioQ.items.size() >= MAX_AUDIO_PACKETS;
        LeaveCriticalSection( &m_lock );
        if( full )
        {
            WaitForSingleObject( m_room, 20 );
            continue;
        }

        FgPacket* pkt = NULL;
        int type = fg_read_packet( m_media, &pkt );
        if( type <= 0 )
        {
            Log::Write( type == 0 ? "End of stream" : "Read error %d - treating as end", type );
            EnterCriticalSection( &m_lock );
            m_videoQ.eof = m_audioQ.eof = true;
            LeaveCriticalSection( &m_lock );
            SetEvent( m_videoData );
            SetEvent( m_audioData );
            eof = true;
            continue;
        }
        if( type == FG_PACKET_VIDEO )
            Push( m_videoQ, m_videoData, pkt );
        else if( type == FG_PACKET_AUDIO && m_hasAudio && !m_speedTest )
            Push( m_audioQ, m_audioData, pkt );
        else
        {
            if( type == FG_PACKET_SUBTITLE )
                AddSubtitle( pkt );
            fg_free_packet( pkt );
        }
    }
}

void FFPlayer::VideoLoop()
{
    if( m_g )
    {
        GpuFeedLoop();
        return;
    }
    bool draining = false;
    while( !m_quit )
    {
        bool eof = false;
        FgPacket* pkt = draining ? NULL : Pop( m_videoQ, m_videoData, eof );
        if( !pkt && !eof && !draining )
            continue;
        if( eof )
            draining = true;

        FgPicture pic;
        int got = fg_decode_video( m_media, pkt, &pic );
        fg_free_packet( pkt );
        if( got <= 0 )
        {
            if( draining )
            {
                InterlockedExchange( &m_ended, 1 );
                while( !m_quit && !m_flushing )
                    Sleep( 10 );
                draining = false;
                if( m_flushing )
                {
                    bool dummy;
                    Pop( m_videoQ, m_videoData, dummy );
                }
            }
            continue;
        }
        InterlockedIncrement( &m_decoded );
        if( m_speedTest )
            continue;

        if( m_reachingFloor )
        {
            if( pic.pts >= 0 && pic.pts < m_seekFloor - 0.02 )
            {
                if( m_catchLevel != 2 )
                {
                    m_catchLevel = 2;
                    m_skippingB = true;
                    fg_set_fast( m_media, 2 );
                }
                continue;
            }
            InterlockedExchange( &m_reachingFloor, 0 );
        }

        double clock = Clock();
        double late = ( clock >= 0 && pic.pts >= 0 ) ? clock - pic.pts : 0.0;
        int level = late > 0.25 ? 2 : ( late > 0.04 ? 1 : 0 );
        if( level < m_catchLevel && late > 0.0 )
            level = m_catchLevel;
        if( level != m_catchLevel )
        {
            if( level == 2 && m_catchLevel < 2 )
                InterlockedIncrement( &m_catchups );
            m_catchLevel = level;
            m_skippingB = level == 2;
            fg_set_fast( m_media, level );
        }
        if( late > 0.5 && GetTickCount() - m_lastShowTick < 500 )
        {
            InterlockedIncrement( &m_dropped );
            continue;
        }

        int slot = -1;
        while( slot < 0 && !m_quit && !m_flushing )
        {
            for( int i = 0; i < FRAME_COUNT && slot < 0; ++i )
                if( !m_frames[i].ready )
                    slot = i;
            if( slot < 0 )
                WaitForSingleObject( m_frameFree, 10 );
        }
        if( slot < 0 )
            continue;

        Frame& f = m_frames[slot];
        int w = pic.width, h = pic.height;
        f.data.resize( w * h + 2 * ( ( w / 2 ) * ( h / 2 ) ) );
        BYTE* dst = &f.data[0];
        for( int plane = 0; plane < 3; ++plane )
        {
            int pw = plane ? w / 2 : w;
            int ph = plane ? h / 2 : h;
            const BYTE* src = pic.plane[plane];
            for( int y = 0; y < ph; ++y, dst += pw, src += pic.pitch[plane] )
                XMemCpy( dst, src, pw );
        }
        f.width = w;
        f.height = h;
        f.pts = pic.pts;
        f.colorspace = pic.colorspace;
        f.fullRange = pic.fullRange;
        InterlockedExchange( &f.ready, 1 );
    }
}

void FFPlayer::AudioLoop()
{
    std::vector<short> pcm( PCM_BYTES / 2 );
    FgInfo info;
    fg_get_info( m_media, &info );

    while( !m_quit )
    {
        bool eof = false;
        FgPacket* pkt = Pop( m_audioQ, m_audioData, eof );
        if( !pkt )
            continue;
        double pts;
        int bytes = fg_decode_audio( m_media, pkt, &pcm[0], PCM_BYTES, &pts );
        fg_free_packet( pkt );
        if( bytes > 0 )
        {
            fg_get_info( m_media, &info );
            int channels = info.channels > 0 ? info.channels : 2;
            if( channels > 8 )
                continue;
            if( pts >= 0 && info.sampleRate > 0 &&
                pts + (double)bytes / ( info.sampleRate * channels * 2 ) < m_seekFloor )
                continue;
            SubmitAudio( &pcm[0], bytes, pts, info.sampleRate, channels );
        }
    }
}

bool FFPlayer::SubmitAudio( const short* pcm, int bytes, double pts, int rate, int channels )
{
    if( !m_xaudio || rate <= 0 )
        return false;

    if( !m_voice || rate != m_voiceRate || channels != m_voiceChannels )
    {
        if( m_voice )
            m_voice->DestroyVoice();
        WAVEFORMATEX wf;
        ZeroMemory( &wf, sizeof( wf ) );
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)channels;
        wf.nSamplesPerSec = rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)( channels * 2 );
        wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
        HRESULT hr = m_xaudio->CreateSourceVoice( &m_voice, &wf, 0, 2.0f );
        if( FAILED( hr ) )
        {
            Log::Write( "CreateSourceVoice failed 0x%08X", hr );
            m_voice = NULL;
            return false;
        }
        m_voiceRate = rate;
        m_voiceChannels = channels;
        m_audioStarted = 0;
        Log::Write( "Audio voice %d Hz %d ch", rate, channels );
    }

    XAUDIO2_VOICE_STATE st;
    for( ;; )
    {
        if( m_quit || m_flushing )
            return false;
        m_voice->GetState( &st );
        if( st.BuffersQueued < AUDIO_BUFFERS - 2 )
            break;
        Sleep( 5 );
    }

    if( bytes > AUDIO_BUFFER_BYTES )
        bytes = AUDIO_BUFFER_BYTES;
    std::vector<BYTE>& buf = m_audioBuf[m_audioNext];
    m_audioNext = ( m_audioNext + 1 ) % AUDIO_BUFFERS;
    buf.resize( bytes );
    memcpy( &buf[0], pcm, bytes );

    XAUDIO2_BUFFER xb;
    ZeroMemory( &xb, sizeof( xb ) );
    xb.AudioBytes = bytes;
    xb.pAudioData = &buf[0];

    if( !m_audioStarted )
    {
        DWORD waitStart = GetTickCount();
        while( m_hasVideo && !m_quit && !m_flushing && !m_ended &&
               GetTickCount() - waitStart < ( m_reachingFloor ? 10000u : 2000u ) )
        {
            int ready = 0;
            for( int i = 0; i < FRAME_COUNT; ++i )
                ready += m_frames[i].ready ? 1 : 0;
            if( ready >= FRAME_COUNT - 1 )
                break;
            if( m_g && m_g->ready >= 2 )
                break;
            Sleep( 5 );
        }
        if( m_quit || m_flushing )
            return false;

        m_voice->GetState( &st );
        m_samplesAtBase = st.SamplesPlayed;
        m_audioBasePts = pts >= 0 ? pts : m_lastPts;

        double first = -1.0;
        EnterCriticalSection( &m_lock );
        for( int i = 0; i < FRAME_COUNT; ++i )
            if( m_frames[i].ready && m_frames[i].pts >= 0 && ( first < 0 || m_frames[i].pts < first ) )
                first = m_frames[i].pts;
        LeaveCriticalSection( &m_lock );
        if( m_g )
            first = m_g->firstPts;
        double gap = first >= 0 && pts >= 0 ? pts - first : 0.0;
        if( gap > 0.1 && gap < 25.0 )
        {
            int loops = (int)ceil( gap / 0.1 );
            UINT32 samples = (UINT32)( gap * rate / loops );
            size_t need = (size_t)samples * channels * 2;
            if( m_silence.size() < need )
                m_silence.assign( 96000 / 10 * 8 * 2 > need ? 96000 / 10 * 8 * 2 : need, 0 );
            XAUDIO2_BUFFER sb;
            ZeroMemory( &sb, sizeof( sb ) );
            sb.AudioBytes = (UINT32)need;
            sb.pAudioData = &m_silence[0];
            sb.LoopLength = samples;
            sb.LoopCount = loops - 1;
            m_voice->SubmitSourceBuffer( &sb );
            m_audioBasePts = pts - (double)samples * loops / rate;
            Log::Write( "Sound starts %.2f s after the picture: padded with silence", gap );
        }
    }
    m_voice->SubmitSourceBuffer( &xb );
    if( !m_audioStarted )
    {
        if( !m_paused )
            m_voice->Start( 0 );
        InterlockedExchange( &m_audioStarted, 1 );
    }
    return true;
}

double FFPlayer::Clock() const
{
    if( !m_opened )
        return -1.0;
    if( m_hasAudio )
    {
        if( !m_audioStarted || !m_voice )
            return -1.0;
        XAUDIO2_VOICE_STATE st;
        m_voice->GetState( &st );
        return m_audioBasePts + (double)( st.SamplesPlayed - m_samplesAtBase ) / m_voiceRate;
    }
    if( m_paused )
        return m_wallBase;
    return m_wallBase + ( GetTickCount() - m_wallStart ) / 1000.0;
}

double FFPlayer::Position() const
{
    if( m_x )
        return m_timeOffset + m_lastPts;
    double c = Clock();
    return m_timeOffset + ( c >= 0 ? c : m_lastPts );
}

void FFPlayer::KeepScreenAwake( bool awake )
{
    if( awake == m_screenSaverOff )
        return;
    XEnableScreenSaver( awake ? FALSE : TRUE );
    m_screenSaverOff = awake;
}

const wchar_t* FFPlayer::AspectName( int mode )
{
    static const wchar_t* const names[ASPECT_COUNT] = { L"Auto", L"Zoom", L"Stretch", L"4:3", L"16:9" };
    return mode >= 0 && mode < ASPECT_COUNT ? names[mode] : names[0];
}

void FFPlayer::Update()
{
    if( m_x )
    {
        XavUpdate();
        KeepScreenAwake( m_active && m_opened && !m_paused );
        return;
    }
    if( m_g )
        GpuPump();
    KeepScreenAwake( m_active && m_opened && !m_paused );
    if( !m_active )
        return;

    if( m_ended && !m_opened && !m_error.empty() )
    {
        Log::Write( "Playback failed: %s", m_error.c_str() );
        Stop();
        return;
    }

    if( m_ended && m_opened )
    {
        bool pending = false;
        for( int i = 0; i < FRAME_COUNT; ++i )
            pending = pending || m_frames[i].ready;
        if( m_g )
            pending = m_g->qn > 0;
        if( !pending )
        {
            if( m_connectionLost )
            {
                m_error = "Connection lost";
                Log::Write( "Playback stopped: connection lost at %.1f s", Position() );
            }
            else
                Log::Write( "Playback finished" );
            Stop();
            return;
        }
    }

    if( GetTickCount() - m_statsTick > 5000 )
    {
        m_statsTick = GetTickCount();
        EnterCriticalSection( &m_lock );
        size_t vq = m_videoQ.items.size(), aq = m_audioQ.items.size();
        LeaveCriticalSection( &m_lock );
        Log::Write( "Stats: pos %.1f, decoded %ld, shown %ld, dropped %ld, B-skip episodes %ld, queues v%u a%u",
                    Position(), m_decoded, m_shown, m_dropped, m_catchups, (unsigned)vq, (unsigned)aq );
        if( m_g )
        {
            long waiting, most;
            GpuH264::Backlog( waiting, most );
            Log::Write( "GPU: fed %ld, taken %ld, queued %d, clock %.2f; 5 s: drawn %ld; GPU backlog %ld (most %ld); late pictures %ld, refreshes missed %ld",
                        m_g->fed, m_g->taken, m_g->qn, Clock(), GpuH264::TakeDrawn(), waiting, most,
                        InterlockedExchange( &s_lateFrames, 0 ), InterlockedExchange( &g_missedBlanks, 0 ) );
        }
    }
}

void FFPlayer::TogglePause()
{
    if( !m_active || !m_opened )
        return;
    m_paused = !m_paused;
    if( m_x )
    {
        XavPause( m_paused );
        return;
    }
    if( m_voice )
    {
        if( m_paused )
            m_voice->Stop( 0 );
        else
            m_voice->Start( 0 );
    }
    if( !m_hasAudio )
    {
        if( m_paused )
            m_wallBase = m_wallBase + ( GetTickCount() - m_wallStart ) / 1000.0;
        m_wallStart = GetTickCount();
    }
}

void FFPlayer::SeekRelative( int seconds )
{
    SeekAbsolute( Position() + seconds );
}

void FFPlayer::SeekAbsolute( double seconds )
{
    if( !m_active || !m_opened || m_seekRequest )
        return;
    double target = seconds - m_timeOffset;
    if( target < 0 )
        target = 0;
    if( m_duration > 0 && target > m_duration - m_timeOffset - 5 )
        target = m_duration - m_timeOffset - 5;
    if( m_x )
    {
        XavSeek( target );
        return;
    }
    m_seekTarget = target;
    InterlockedExchange( &m_seekRequest, 1 );
    WakeAll();
}

void FFPlayer::EnsureTextures( int w, int h )
{
    if( w == m_texW && h == m_texH && m_tex[0] )
        return;
    for( int i = 0; i < 3; ++i )
    {
        if( m_tex[i] )
            m_tex[i]->Release();
        m_tex[i] = NULL;
        int tw = i ? w / 2 : w;
        int th = i ? h / 2 : h;
        m_device->CreateTexture( tw, th, 1, 0, D3DFMT_LIN_L8, 0, &m_tex[i], NULL );
    }
    m_texW = w;
    m_texH = h;
    Log::Write( "Video textures %dx%d", w, h );
}

void FFPlayer::Upload( const Frame& f )
{
    EnsureTextures( f.width, f.height );
    const BYTE* src = &f.data[0];
    for( int i = 0; i < 3; ++i )
    {
        if( !m_tex[i] )
            return;
        int pw = i ? f.width / 2 : f.width;
        int ph = i ? f.height / 2 : f.height;
        m_tex[i]->BlockUntilNotBusy();
        D3DLOCKED_RECT lr;
        if( FAILED( m_tex[i]->LockRect( 0, &lr, NULL, 0 ) ) )
            return;
        BYTE* dst = (BYTE*)lr.pBits;
        for( int y = 0; y < ph; ++y, dst += lr.Pitch, src += pw )
            memcpy( dst, src, pw );
        m_tex[i]->UnlockRect( 0 );
    }
    m_frameW = f.width;
    m_frameH = f.height;
    m_frameColorspace = f.colorspace;
    m_frameFullRange = f.fullRange;
    m_haveFrame = true;
}

void FFPlayer::RenderFrame( const D3DRECT& screen )
{
    if( !m_active )
        return;
    if( m_x )
    {
        XavRender( screen );
        return;
    }

    double clock = Clock();
    if( m_g )
        GpuSelect( clock );
    else
    {
    EnterCriticalSection( &m_lock );
    int pick = -1;
    for( int i = 0; i < FRAME_COUNT; ++i )
    {
        if( !m_frames[i].ready )
            continue;
        bool due = clock < 0 ? !m_haveFrame : m_frames[i].pts <= clock + 0.005;
        if( !due )
            continue;
        if( pick < 0 )
            pick = i;
        else
        {
            int older = m_frames[i].pts < m_frames[pick].pts ? i : pick;
            if( older == pick )
                pick = i;
            m_frames[older].ready = 0;
            InterlockedIncrement( &m_dropped );
        }
    }
    if( pick >= 0 )
    {
        Upload( m_frames[pick] );
        m_lastPts = m_frames[pick].pts;
        m_lastShowTick = GetTickCount();
        m_frames[pick].ready = 0;
        InterlockedIncrement( &m_shown );
    }
    LeaveCriticalSection( &m_lock );
    if( pick >= 0 )
        SetEvent( m_frameFree );
    }

    if( !m_haveFrame )
        return;

    float sw = (float)( screen.x2 - screen.x1 ), sh = (float)( screen.y2 - screen.y1 );
    float picture = (float)( m_frameW * m_pixelAspect / m_frameH );
    if( m_aspect == ASPECT_4_3 )  picture = 4.0f / 3.0f;
    if( m_aspect == ASPECT_16_9 ) picture = 16.0f / 9.0f;
    float aspect = picture * ( sw / sh ) / m_tvAspect;
    float w = sw, h = sw / aspect;
    if( m_aspect == ASPECT_STRETCH )
        h = sh;
    else if( m_aspect == ASPECT_ZOOM ? h < sh : h > sh )
    {
        h = sh;
        w = sh * aspect;
    }
    float x0 = ( sw - w ) / sw - 1.0f, x1 = x0 + 2.0f * w / sw;
    float y0 = 1.0f - ( sh - h ) / sh, y1 = y0 - 2.0f * h / sh;
    QuadVertex quad[4] =
    {
        { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 },
        { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 },
    };

    bool hd = m_frameColorspace == FG_CS_BT709 ||
              ( m_frameColorspace == FG_CS_UNKNOWN && ( m_frameW >= 1024 || m_frameH >= 576 ) );
    const float( *m )[4] = hd ? ( m_frameFullRange ? BT709_FULL : BT709 ) : ( m_frameFullRange ? BT601_FULL : BT601 );
    float levels[4] = { m_frameFullRange ? 0.0f : 0.0625f, m_brightness, 0, 0 };
    m_device->SetVertexDeclaration( m_decl );
    m_device->SetVertexShader( m_vs );
    m_device->SetPixelShader( m_ps );
    m_device->SetPixelShaderConstantF( 0, &m[0][0], 3 );
    m_device->SetPixelShaderConstantF( 3, levels, 1 );
    for( int i = 0; i < 3; ++i )
    {
        IDirect3DBaseTexture9* tex = m_tex[i];
        if( m_g )
            tex = &m_g->tex[i];
        m_device->SetTexture( i, tex );
        m_device->SetSamplerState( i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
        m_device->SetSamplerState( i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
        m_device->SetSamplerState( i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
        m_device->SetSamplerState( i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    }
    if( m_g )
    {
        const GpuH264::Frame& c = m_g->cur;
        float u0 = (float)c.cropX / c.pitchY, u1 = (float)( c.cropX + c.showW ) / c.pitchY;
        float v0 = (float)c.cropY / c.heightY, v1 = (float)( c.cropY + c.showH ) / c.heightY;
        quad[0].u = quad[2].u = u0;
        quad[1].u = quad[3].u = u1;
        quad[0].v = quad[1].v = v0;
        quad[2].v = quad[3].v = v1;
    }
    m_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    m_device->SetRenderState( D3DRS_ZENABLE, FALSE );
    m_device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    m_device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( QuadVertex ) );
    for( int i = 0; i < 3; ++i )
        m_device->SetTexture( i, NULL );
    m_device->SetVertexShader( NULL );
    m_device->SetPixelShader( NULL );
}

std::wstring FFPlayer::StatusText() const
{
    if( !m_active )
        return m_error.empty() ? L"" : std::wstring( m_error.begin(), m_error.end() );
    std::wstring via = m_current < m_labels.size() ? L"  [" + m_labels[m_current] + L"]" : L"";
    if( !m_opened )
        return L"Opening video..." + via;
    if( m_seekRequest || ( !m_haveFrame ) )
        return L"Buffering...  " + m_info + via;
    return ( m_paused ? L"Paused  " : L"" ) + FormatTime( Position() ) + L" / " + FormatTime( m_duration ) +
           L"   " + m_info + via;
}

std::wstring FFPlayer::ColorInfo() const
{
    bool hd = m_frameColorspace == FG_CS_BT709 ||
              ( m_frameColorspace == FG_CS_UNKNOWN && ( m_frameW >= 1024 || m_frameH >= 576 ) );
    std::wstring s = hd ? L"BT.709" : L"BT.601";
    if( m_frameColorspace == FG_CS_UNKNOWN )
        s += L" (guessed)";
    s += m_frameFullRange ? L", full range" : L", TV range";
    return s;
}
std::wstring FFPlayer::MediaInfo() const
{
    std::wstring via = m_current < m_labels.size() ? L"  [" + m_labels[m_current] + L"]" : L"";
    return m_info + via;
}

std::wstring FFPlayer::DecodeInfo() const
{
    int w = m_videoW, h = m_videoH;
    const wchar_t* gpu = NULL;
    if( m_x )
    {
        w = m_haveFrame ? m_frameW : 0;
        h = m_haveFrame ? m_frameH : 0;
        gpu = L"GPU, XAV";
    }
    else if( m_g )
        gpu = L"GPU";
    if( w <= 0 || h <= 0 )
        return L"";
    wchar_t res[16], buf[48];
    if( w > 1920 || h > 1088 )
        swprintf_s( res, L"%dx%d", w, h );
    else
    {
        bool wide = w * 9 >= h * 16 - 16;
        swprintf_s( res, L"%dp", wide && w > 1280 ? 1080 : wide && w >= 1200 ? 720 : h );
    }
    if( gpu )
        swprintf_s( buf, L"%s (%s)", res, gpu );
    else
        swprintf_s( buf, L"%s (CPU, %d threads)", res, m_decodeThreads );
    return buf;
}

void FFPlayer::GetStats( Stats& s )
{
    s.decoded = m_decoded;
    s.shown = m_shown;
    s.dropped = m_dropped;
    s.catchups = m_catchups;
    s.bytesRead = m_bytesRead;
    s.width = m_frameW;
    s.height = m_frameH;
    s.skippingB = m_skippingB;
    EnterCriticalSection( &m_lock );
    s.videoPackets = (int)m_videoQ.items.size();
    s.audioPackets = (int)m_audioQ.items.size();
    LeaveCriticalSection( &m_lock );
    s.audioBufferedMs = 0;
    if( m_voice && m_voiceRate > 0 )
    {
        XAUDIO2_VOICE_STATE st;
        m_voice->GetState( &st );
        s.audioBufferedMs = (int)st.BuffersQueued * 32;
    }
}
void FFPlayer::AddSubtitle( FgPacket* pkt )
{
    char text[1024];
    double start, duration;
    if( !fg_subtitle( m_media, pkt, text, sizeof( text ), &start, &duration ) )
        return;
    SubtitleCue cue;
    cue.start = start;
    cue.end = start + duration;
    cue.text = text;
    EnterCriticalSection( &m_lock );
    size_t i = m_cues.size();
    while( i > 0 && m_cues[i - 1].start > start )
        --i;
    bool repeat = false;
    for( size_t k = i; k > 0 && m_cues[k - 1].start == start && !repeat; --k )
        repeat = m_cues[k - 1].text == cue.text;
    if( !repeat )
        m_cues.insert( m_cues.begin() + i, cue );
    LeaveCriticalSection( &m_lock );
}

std::vector<FgTrack> FFPlayer::Tracks()
{
    EnterCriticalSection( &m_lock );
    std::vector<FgTrack> out = m_tracks;
    LeaveCriticalSection( &m_lock );
    return out;
}

void FFPlayer::SetSubtitleTrack( int index )
{
    m_subWanted = index;
    InterlockedExchange( &m_subChanged, 1 );
}

void FFPlayer::SetExternalSubtitles( const std::vector<SubtitleCue>& cues )
{
    SetSubtitleTrack( -1 );
    EnterCriticalSection( &m_lock );
    m_external = cues;
    LeaveCriticalSection( &m_lock );
}

std::string FFPlayer::SubtitleAt( double seconds )
{
    double t = seconds - m_timeOffset;
    std::string out;
    EnterCriticalSection( &m_lock );
    const std::vector<SubtitleCue>& cues = m_external.empty() ? m_cues : m_external;
    for( size_t i = 0; i < cues.size() && cues[i].start <= t; ++i )
        if( t < cues[i].end )
        {
            if( !out.empty() )
                out += "\n";
            out += cues[i].text;
        }
    LeaveCriticalSection( &m_lock );
    return out;
}

#include "FFPlayerGpu.inc"
#include "FFPlayerXav.inc"
