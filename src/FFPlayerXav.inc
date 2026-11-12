extern volatile LONG g_vblanks;
extern DWORD g_waitMs;

namespace
{
    const DWORD XAV_PREFILL_MS = 1000;
    const DWORD XAV_LOG_MS = 30000;

    const char* PS_RGB_SOURCE =
        "sampler2D tex : register( s0 );\n"
        "float4 levels : register( c3 );\n"
        "float4 main( float2 uv : TEXCOORD0 ) : COLOR { return float4( tex2D( tex, uv ).rgb + levels.y, 1.0 ); }\n";

    bool XavExtension( const std::string& url )
    {
        size_t dot = url.find_last_of( '.' );
        if( dot == std::string::npos )
            return false;
        std::string ext = url.substr( dot + 1 );
        for( size_t i = 0; i < ext.size(); ++i )
            ext[i] = (char)tolower( (unsigned char)ext[i] );
        return ext == "wmv";
    }
}

struct FFPlayer::XavState : public IXAVPlayerEvents
{
    IXAVPlayer*            player;
    IXAVPlayerGetTexture*  frames;
    volatile LONG          state;
    volatile LONG          width, height;
    volatile LONG          errorHr;
    volatile __int64       duration;
    bool                   played;
    bool                   startSeekDone;
    DWORD                  openTick, lastPlayTick, statsTick, endTick;
    DWORD                  seekTick;
    double                 startSeconds;
    LONGLONG               lastStamp;
    LONG                   lastVb;
    long                   held[5];
    LONG                   loopVb;
    long                   loops[5];
    DWORD                  oldCore;
    bool                   movedCore;

    XavState() : player( NULL ), frames( NULL ), state( -1 ), width( 0 ), height( 0 ), errorHr( 0 ), duration( 0 ),
                 played( false ), startSeekDone( false ), openTick( GetTickCount() ), lastPlayTick( 0 ),
                 statsTick( GetTickCount() ), endTick( 0 ), seekTick( 0 ), startSeconds( 0 ), lastStamp( -1 ), lastVb( 0 ), loopVb( 0 ), oldCore( 0 ), movedCore( false ) { memset( held, 0, sizeof( held ) ); memset( loops, 0, sizeof( loops ) ); }

    STDMETHOD( QueryInterface )( REFIID, void** out ) { *out = this; return S_OK; }
    STDMETHOD_( ULONG, AddRef )() { return 1; }
    STDMETHOD_( ULONG, Release )() { return 1; }
    void StatusChanged( const IXAVPlayerStatus* status )
    {
        LONG s = (LONG)status->GetState();
        if( s != state )
            Log::Write( "XAV: state %ld (%lu x %lu, %.1f s)", s, status->GetContentResolutionWidth(),
                        status->GetContentResolutionHeight(), status->GetDuration() / 1e7 );
        if( status->GetContentResolutionWidth() )
        {
            width = (LONG)status->GetContentResolutionWidth();
            height = (LONG)status->GetContentResolutionHeight();
        }
        if( status->GetDuration() )
            duration = (__int64)status->GetDuration();
        if( s == XAVPlayerStateError )
        {
            XAVPlayerAlert first;
            status->GetFirstAlert( &first );
            errorHr = first.hrAlert ? (LONG)first.hrAlert : (LONG)E_FAIL;
            Log::Write( "XAV: error, component %d type %lu hr %08lX", (int)first.Component, first.dwType, first.hrAlert );
        }
        state = s;
    }
    void PositionUpdate( const IXAVPlayerStatus* ) {}
    void Alert( XAVPlayerAlert* alert, const IXAVPlayerStatus* )
    {
        Log::Write( "XAV: alert level %d component %d type %lu hr %08lX", (int)alert->Level, (int)alert->Component,
                    alert->dwType, alert->hrAlert );
    }

    double Stat( DWORD key )
    {
        IXAVPlayerStats* stats = NULL;
        double v = -1;
        if( SUCCEEDED( player->GetStats( &stats ) ) && stats )
        {
            XAVPropertyValue pv;
            if( stats->GetValue( key, &pv ) == S_OK )
                v = pv.m_Type == XAVPropertyValue::DOUBLEProperty ? pv.m_Value.flValue : (double)pv.m_Value.uInt32Value;
            stats->Release();
        }
        return v;
    }
};

bool FFPlayer::XavOpen()
{
    if( !m_gpuWanted || !GpuH264::Available() || !GpuH264::XavWanted() || m_urls.size() != 1 ||
        m_urls[0].compare( 0, 4, "http" ) == 0 || !XavExtension( m_urls[0] ) )
        return false;
    if( !m_psRgb )
    {
        ID3DXBuffer* code = NULL;
        ID3DXBuffer* errors = NULL;
        if( SUCCEEDED( D3DXCompileShader( PS_RGB_SOURCE, (UINT)strlen( PS_RGB_SOURCE ), NULL, NULL, "main", "ps_3_0",
                                          0, &code, &errors, NULL ) ) )
        {
            m_device->CreatePixelShader( (const DWORD*)code->GetBufferPointer(), &m_psRgb );
            code->Release();
        }
        if( errors )
            errors->Release();
        if( !m_psRgb )
            return false;
    }
    XavState* x = new XavState;
    HRESULT hr = CreateXAVPlayer( &x->player, x, NULL );
    if( FAILED( hr ) || !x->player )
    {
        Log::Write( "XAV: CreateXAVPlayer %08lX - FFmpeg plays it", hr );
        delete x;
        return false;
    }
    IXAVPropertyStore* props = NULL;
    if( SUCCEEDED( x->player->GetPlayerPropertyStore( &props ) ) && props )
    {
        XAVPropertyValue pv;
        pv.m_Type = XAVPropertyValue::BOOLProperty;
        pv.m_Value.fValue = FALSE;
        props->SetValue( XAVOpenParamAutoPlay, &pv );
        pv.m_Value.fValue = FALSE;
        props->SetValue( XAVOpenParamAutoDownloadCodecPack, &pv );
        props->Release();
    }
    x->startSeconds = m_startSeconds;
    hr = x->player->Open( m_urls[0].c_str() );
    if( SUCCEEDED( hr ) )
        hr = x->player->GetPlayerFrameInfo( &x->frames );
    if( FAILED( hr ) || !x->frames )
    {
        Log::Write( "XAV: Open %08lX - FFmpeg plays it", hr );
        x->player->Shutdown();
        x->player->Release();
        delete x;
        return false;
    }
    m_x = x;
    m_info = L"XAV (system player)";
    x->oldCore = GetCurrentProcessorNumber();
    XSetThreadProcessor( GetCurrentThread(), GpuH264::RENDER_CORE );
    x->movedCore = true;
    Log::Write( "XAV: opening %s", m_urls[0].c_str() );
    return true;
}

void FFPlayer::XavStop()
{
    XavState* x = m_x;
    if( !x )
        return;
    m_x = NULL;
    if( x->movedCore )
        XSetThreadProcessor( GetCurrentThread(), x->oldCore );
    if( x->frames )
        x->frames->Release();
    x->player->Stop();
    x->player->Shutdown();
    x->player->Release();
    Log::Write( "XAV: closed" );
    delete x;
    m_haveFrame = false;
}

void FFPlayer::XavFallBack()
{
    Log::Write( "XAV: could not play it (%08lX) - FFmpeg takes over", m_x ? m_x->errorHr : 0 );
    XavStop();
    m_error.clear();
    m_opened = 0;
    m_threads[0] = StartThread( DemuxThread, this, DEMUX_CPU );
}

void FFPlayer::XavUpdate()
{
    XavState* x = m_x;
    LONG s = x->state;
    if( s == XAVPlayerStateError )
    {
        if( !x->played )
            XavFallBack();
        else
        {
            char text[64];
            sprintf_s( text, "Playback error %08lX", x->errorHr );
            m_error = text;
            Stop();
        }
        return;
    }
    if( !x->played && GetTickCount() - x->openTick > 30000 )
    {
        Log::Write( "XAV: did not start in 30 s - stopping" );
        m_error = "Playback did not start. Restart the player and try again.";
        Stop();
        return;
    }
    if( !x->played && s == XAVPlayerStateStopped && !x->lastPlayTick )
        x->lastPlayTick = GetTickCount() - 3000 + XAV_PREFILL_MS;
    if( !x->played && s == XAVPlayerStateStopped && GetTickCount() - x->lastPlayTick > 3000 )
    {
        x->lastPlayTick = GetTickCount();
        Log::Write( "XAV: Play %08lX", x->player->Play() );
    }
    if( s == XAVPlayerStatePlaying && !x->played )
    {
        x->played = true;
        InterlockedExchange( &m_opened, 1 );
        if( x->startSeconds > 1 && !x->startSeekDone )
            x->player->SeekAbsolute( (ULONGLONG)( x->startSeconds * 1e7 ) );
        x->startSeekDone = true;
    }
    if( x->duration )
        m_duration = m_timeOffset + x->duration / 1e7;
    if( x->width )
    {
        m_frameW = x->width;
        m_frameH = x->height;
    }
    IXAVPlayerStatus* st = NULL;
    if( SUCCEEDED( x->player->GetStatus( &st ) ) && st )
    {
        double reported = st->GetPosition() / 1e7;
        if( x->seekTick && ( fabs( reported - m_lastPts ) < 1.5 || GetTickCount() - x->seekTick > 3000 ) )
            x->seekTick = 0;
        if( x->played && !x->seekTick )
            m_lastPts = reported;
        double len = x->duration / 1e7;
        if( x->duration && m_lastPts >= len - 0.3 )
        {
            if( !x->endTick )
                x->endTick = GetTickCount();
        }
        else
            x->endTick = 0;
        bool ended = x->played && ( st->IsEndOfMedia() ||
                                    ( s == XAVPlayerStateStopped && x->duration && m_lastPts > len - 2 ) ||
                                    ( x->endTick && GetTickCount() - x->endTick > 1500 ) );
        st->Release();
        if( ended )
        {
            Log::Write( "Playback finished" );
            Stop();
            return;
        }
    }
    if( GetTickCount() - x->statsTick > XAV_LOG_MS )
    {
        x->statsTick = GetTickCount();
        double dropped = x->Stat( XAVStatsTotalDroppedFrames );
        if( dropped >= 0 )
            m_dropped = (long)dropped;
        Log::Write( "XAV: pos %.1f, fps %.1f, dropped %ld, frame rate %.2f; pictures held 1/2/3/4+ blanks: %ld %ld %ld %ld", Position(),
                    x->Stat( XAVStatsActualFramesPerSecond ), m_dropped, x->Stat( XAVStatsFrameRate ),
                    x->held[1], x->held[2], x->held[3], x->held[4] );
        Log::Write( "XAV: render loops took 0/1/2/3/4+ blanks: %ld %ld %ld %ld %ld; waited %lu ms of 5000", x->loops[0], x->loops[1], x->loops[2], x->loops[3], x->loops[4], g_waitMs );
        g_waitMs = 0;
        memset( x->held, 0, sizeof( x->held ) );
        memset( x->loops, 0, sizeof( x->loops ) );
    }
}

void FFPlayer::XavPause( bool pause )
{
    if( m_x )
        Log::Write( "XAV: %s %08lX", pause ? "Pause" : "Play", pause ? m_x->player->Pause() : m_x->player->Play() );
}

void FFPlayer::XavSeek( double seconds )
{
    if( m_x )
    {
        m_lastPts = seconds;
        m_x->seekTick = GetTickCount();
        Log::Write( "XAV: seek to %.1f: %08lX", seconds, m_x->player->SeekAbsolute( (ULONGLONG)( seconds * 1e7 ) ) );
    }
}

void FFPlayer::XavRender( const D3DRECT& screen )
{
    XavState* x = m_x;
    LONG s = x->state;
    if( s != XAVPlayerStateStarting && s != XAVPlayerStatePlaying && s != XAVPlayerStatePaused )
        return;
    {
        LONG vb = g_vblanks;
        if( x->loopVb )
        {
            LONG d = vb - x->loopVb;
            x->loops[d < 0 ? 0 : d > 4 ? 4 : d]++;
        }
        x->loopVb = vb;
    }
    IDirect3DTexture9* tex = NULL;
    XAVRenderResultInfo info;
    if( FAILED( x->frames->GetTexture( FALSE, &tex, &info ) ) || !tex )
        return;
    if( !m_haveFrame )
        Log::Write( "XAV: first picture %ld x %ld", info.resultRect.right - info.resultRect.left,
                    info.resultRect.bottom - info.resultRect.top );
    m_haveFrame = true;
    if( info.llTimeStamp != x->lastStamp )
    {
        LONG vb = g_vblanks;
        if( x->lastStamp >= 0 && x->lastVb )
        {
            LONG d = vb - x->lastVb;
            x->held[d < 1 ? 1 : d > 4 ? 4 : d]++;
        }
        x->lastVb = vb;
        x->lastStamp = info.llTimeStamp;
        InterlockedIncrement( &m_shown );
    }
    D3DSURFACE_DESC d;
    tex->GetLevelDesc( 0, &d );
    int pw = info.resultRect.right - info.resultRect.left, ph = info.resultRect.bottom - info.resultRect.top;
    if( pw <= 0 || ph <= 0 )
    {
        pw = d.Width;
        ph = d.Height;
    }
    m_pixelAspect = info.dwPixelAspectRatioNumerator && info.dwPixelAspectRatioDenominator
                        ? (double)info.dwPixelAspectRatioNumerator / info.dwPixelAspectRatioDenominator : 1.0;

    float sw = (float)( screen.x2 - screen.x1 ), sh = (float)( screen.y2 - screen.y1 );
    float picture = (float)( pw * m_pixelAspect / ph );
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
    float u0 = (float)info.resultRect.left / d.Width, u1 = (float)( info.resultRect.left + pw ) / d.Width;
    float v0 = (float)info.resultRect.top / d.Height, v1 = (float)( info.resultRect.top + ph ) / d.Height;
    QuadVertex quad[4] =
    {
        { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 },
        { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 },
    };
    float levels[4] = { 0, m_brightness, 0, 0 };
    m_device->SetVertexDeclaration( m_decl );
    m_device->SetVertexShader( m_vs );
    m_device->SetPixelShader( m_psRgb );
    m_device->SetPixelShaderConstantF( 3, levels, 1 );
    m_device->SetTexture( 0, tex );
    m_device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    m_device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    m_device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    m_device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    m_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    m_device->SetRenderState( D3DRS_ZENABLE, FALSE );
    m_device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    m_device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( QuadVertex ) );
    m_device->SetTexture( 0, NULL );
    m_device->SetVertexShader( NULL );
    m_device->SetPixelShader( NULL );
    tex->Release();
}
