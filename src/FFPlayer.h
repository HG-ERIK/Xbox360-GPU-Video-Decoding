// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>
#include <string>
#include <vector>
#include <deque>
#include "ffglue.h"

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;
namespace Http { class Stream; }

struct SubtitleCue
{
    double      start, end;
    std::string text;
};

class FFPlayer
{
public:
    FFPlayer();
    ~FFPlayer();

    bool Startup( IDirect3DDevice9* device );

    void Open( const std::vector<std::string>& urls, const std::vector<std::wstring>& labels,
               const std::string& headers, double startSeconds = 0.0, double timeOffset = 0.0,
               double durationHint = 0.0 );
    void Stop();
    bool IsActive() const { return m_active; }

    void Update();
    void TogglePause();
    void SeekRelative( int seconds );
    void SeekAbsolute( double seconds );
    bool IsPaused() const { return m_paused; }

    void SetSpeedTest( bool on ) { m_speedTest = on; }
    bool IsSeeking() const { return m_seekRequest != 0; }
    bool IsBuffering() const { return m_active && ( !m_opened || !m_haveFrame || m_seekRequest ); }

    void RenderFrame( const D3DRECT& screen );

    std::wstring StatusText() const;

    struct Stats
    {
        long    decoded, shown, dropped, catchups;
        int     videoPackets, audioPackets;
        int     audioBufferedMs;
        __int64 bytesRead;
        int     width, height;
        bool    skippingB;
    };
    void         GetStats( Stats& s );
    std::wstring MediaInfo() const;
    std::wstring DecodeInfo() const;
    bool         Failed() const { return !m_error.empty() && !m_active; }
    const std::string& Error() const { return m_error; }
    double       Position() const;
    double       Duration() const { return m_duration; }
    bool         CanSeek() const  { return m_opened && m_streamSize >= 0; }

    std::vector<FgTrack> Tracks();
    void         SetSubtitleTrack( int index );
    void         SetExternalSubtitles( const std::vector<SubtitleCue>& cues );
    std::string  SubtitleAt( double seconds );

private:
    static DWORD WINAPI DemuxThread( LPVOID p );
    static DWORD WINAPI VideoThread( LPVOID p );
    static DWORD WINAPI AudioThread( LPVOID p );
    void  DemuxLoop();
    void  VideoLoop();
    void  AudioLoop();
    bool  OpenMedia( std::string& error );
    bool  OpenStream( std::string& error );
    void  AddSubtitle( FgPacket* pkt );
    void  DoSeek( double seconds );

    static int SourceRead( void* opaque, unsigned char* buf, int size );
    static int SourceSeek( void* opaque, __int64 pos );

    struct PacketQueue
    {
        std::deque<FgPacket*> items;
        bool                  eof;
        PacketQueue() : eof( false ) {}
    };
    FgPacket*  Pop( PacketQueue& q, HANDLE dataEvent, bool& eof );
    void       Push( PacketQueue& q, HANDLE dataEvent, FgPacket* p );
    void       ClearQueue( PacketQueue& q );

    struct Frame
    {
        std::vector<BYTE> data;
        int               width, height;
        double            pts;
        int               colorspace, fullRange;
        volatile LONG     ready;
    };
    enum { FRAME_COUNT = 4 };

    bool   SubmitAudio( const short* pcm, int bytes, double pts, int rate, int channels );
    double Clock() const;

    bool   CreateShaders();
    void   EnsureTextures( int w, int h );
    void   Upload( const Frame& f );

    IDirect3DDevice9*          m_device;
    IDirect3DVertexShader9*    m_vs;
    IDirect3DPixelShader9*     m_ps;
    IDirect3DVertexDeclaration9* m_decl;
    IDirect3DTexture9*         m_tex[3];
    int                        m_texW, m_texH;
    bool                       m_haveFrame;
    int                        m_frameW, m_frameH;
    int                        m_videoW, m_videoH, m_decodeThreads;
    int                        m_frameColorspace, m_frameFullRange;
    float                      m_brightness;
    int                        m_aspect;
    double                     m_pixelAspect;
    float                      m_tvAspect;
    bool                       m_screenSaverOff;

    void   KeepScreenAwake( bool awake );

public:
    enum { ASPECT_AUTO, ASPECT_ZOOM, ASPECT_STRETCH, ASPECT_4_3, ASPECT_16_9, ASPECT_COUNT };
    static const wchar_t* AspectName( int mode );
    void   SetAspect( int mode )    { m_aspect = mode >= 0 && mode < ASPECT_COUNT ? mode : ASPECT_AUTO; }
    int    Aspect() const           { return m_aspect; }

    void   SetBrightness( float b ) { m_brightness = b; }
    float  Brightness() const       { return m_brightness; }
    std::wstring ColorInfo() const;

    void   SetGpuWanted( bool on )  { m_gpuWanted = on; }
    bool   GpuWanted() const        { return m_gpuWanted; }
    bool   GpuMode() const          { return m_g != NULL; }
    void   SetGpuOnly( bool on )    { m_gpuOnly = on; }
    bool   XavMode() const          { return m_x != NULL; }
private:
    struct XavState;
    XavState*                  m_x;
    IDirect3DPixelShader9*     m_psRgb;
    bool   XavOpen();
    void   XavStop();
    void   XavUpdate();
    void   XavRender( const D3DRECT& screen );
    void   XavPause( bool pause );
    void   XavSeek( double seconds );
    void   XavFallBack();
    struct GpuState;
    GpuState*                  m_g;
    bool                       m_gpuWanted;
    bool                       m_gpuOnly;
    void   GpuMaybeStart( const FgInfo& info );
    void   GpuFeedLoop();
    void   GpuFlush();
    void   GpuClose();
    void   GpuPump();
    void   GpuTake();
    void   GpuSelect( double clock );
    static void GpuRead( void* ctx, unsigned char** data, unsigned long* length, unsigned char* info );

    IXAudio2*                  m_xaudio;
    IXAudio2MasteringVoice*    m_master;
    IXAudio2SourceVoice*       m_voice;
    int                        m_voiceRate, m_voiceChannels;
    enum { AUDIO_BUFFERS = 12, AUDIO_BUFFER_BYTES = 192000 * 2 };
    std::vector<BYTE>          m_audioBuf[AUDIO_BUFFERS];
    std::vector<BYTE>          m_silence;
    int                        m_audioNext;
    UINT64                     m_samplesAtBase;
    double                     m_audioBasePts;
    volatile LONG              m_audioStarted;

    CRITICAL_SECTION           m_lock;
    HANDLE                     m_videoData;
    HANDLE                     m_audioData;
    HANDLE                     m_room;
    HANDLE                     m_frameFree;
    void                       WakeAll();
    HANDLE                     m_threads[3];
    volatile LONG              m_quit;
    volatile LONG              m_seekRequest;
    double                     m_seekTarget;
    volatile LONG              m_flushing;
    volatile LONG              m_decodersParked;
    PacketQueue                m_videoQ, m_audioQ;
    Frame                      m_frames[FRAME_COUNT];

    std::vector<std::string>   m_urls;
    std::vector<std::wstring>  m_labels;
    std::string                m_headers;
    size_t                     m_current;
    Http::Stream*              m_stream;
    std::vector<FgTrack>       m_tracks;
    std::vector<SubtitleCue>   m_cues;
    std::vector<SubtitleCue>   m_external;
    volatile LONG              m_subWanted;
    volatile LONG              m_subChanged;
    HANDLE                     m_file;
    __int64                    m_streamPos;
    __int64                    m_streamSize;
    FgMedia*                   m_media;
    bool                       m_hasAudio;
    bool                       m_hasVideo;
    volatile DWORD             m_lastShowTick;
    double                     m_duration;
    double                     m_startSeconds;
    double                     m_timeOffset;

    bool                       m_active;
    volatile LONG              m_opened;
    volatile LONG              m_ended;
    bool                       m_paused;
    DWORD                      m_wallStart;
    double                     m_wallBase;
    double                     m_lastPts;
    std::string                m_error;
    std::wstring               m_info;

    volatile LONG              m_decoded, m_dropped, m_shown, m_catchups;
    volatile __int64           m_bytesRead;
    bool                       m_skippingB;
    int                        m_catchLevel;
    volatile double            m_seekFloor;
    volatile LONG              m_reachingFloor;
    volatile LONG              m_connectionLost;
    bool                       m_speedTest;
    DWORD                      m_statsTick;
};
