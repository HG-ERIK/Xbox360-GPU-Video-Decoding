extern volatile LONG g_swapBlank;

static const double STAMP_SHIFT = 100.0;
static const __int64 STAMP_GEN = 100000LL * 10000000LL;
static __int64 GpuStamp( double t, LONG gen ) { return (__int64)( ( t + STAMP_SHIFT ) * 10000000.0 + 0.5 ) + ( gen & 1023 ) * STAMP_GEN; }
static double GpuStampTime( __int64 s ) { return ( s % STAMP_GEN ) / 10000000.0 - STAMP_SHIFT; }

static bool GpuDisposable( const unsigned char* b, int size )
{
    for( int i = 0; i + 3 < size; ++i )
    {
        if( b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 1 )
            continue;
        int type = b[i + 3] & 31;
        if( type == 1 || type == 5 )
            return type == 1 && ( b[i + 3] & 0x60 ) == 0;
        i += 2;
    }
    return false;
}

void FFPlayer::GpuMaybeStart( const FgInfo& info )
{
    if( !m_gpuWanted || !GpuH264::Available() || !info.hasVideo || strcmp( info.videoCodec, "h264" ) != 0 ||
        info.width > 1920 || info.height > 1088 || info.profile > 100 )
        return;
    GpuState* g = new GpuState;
    g->level = info.level;
    g->frameDur = info.fps > 1.0 ? 1.0 / info.fps : 1.0 / 25;
    m_g = g;
    m_info += L"  [GPU]";
    Log::Write( "GPU decoding for this file (level %d)", info.level );
}

void FFPlayer::GpuRead( void* ctx, unsigned char** data, unsigned long* length, unsigned char* info )
{
    FFPlayer* p = (FFPlayer*)ctx;
    for( ;; )
    {
        GpuState* g = p->m_g;
        if( !g || p->m_quit || GpuH264::Closing() )
            return;
        EnterCriticalSection( &g->lock );
        if( !g->chunks.empty() )
        {
            g->stage.clear();
            g->stage.swap( g->chunks.front() );
            g->chunks.pop_front();
            __int64 stamp = g->chunkStamps.front();
            g->chunkStamps.pop_front();
            if( info )
            {
                *(__int64*)( info + 8 ) = stamp;
                *(DWORD*)( info + 0x10 ) = 1;
            }
            static bool first = true;
            if( first )
            {
                first = false;
                Log::Write( "GPU: decoder reads its first bytes (%u)", (unsigned)g->stage.size() );
            }
            *data = &g->stage[0];
            *length = (unsigned long)g->stage.size();
            LeaveCriticalSection( &g->lock );
            return;
        }
        LeaveCriticalSection( &g->lock );
        WaitForSingleObject( g->dataEvent, 10 );
    }
}

void FFPlayer::GpuFeedLoop()
{
    GpuState& g = *m_g;
    bool draining = false;
    while( !m_quit )
    {
        bool eof = false;
        FgPacket* pkt = draining ? NULL : Pop( m_videoQ, m_videoData, eof );
        if( !pkt )
        {
            if( eof )
                draining = true;
            if( !draining )
                continue;
            long last = -1;
            DWORD since = GetTickCount();
            while( !m_quit && !m_flushing && g.taken < g.fed - 3 && GetTickCount() - since < 3000 )
            {
                if( g.taken != last )
                {
                    last = g.taken;
                    since = GetTickCount();
                }
                Sleep( 10 );
            }
            InterlockedExchange( &m_ended, 1 );
            while( !m_quit && !m_flushing )
                Sleep( 10 );
            draining = false;
            if( m_flushing )
            {
                bool dummy;
                Pop( m_videoQ, m_videoData, dummy );
            }
            continue;
        }

        double t = fg_packet_time( m_media, pkt );
        if( !fg_packet_has_time( pkt ) )
            t = g.lastT + g.frameDur;
        for( ;; )
        {
            if( m_quit || m_flushing )
                break;
            double clock = Clock();
            long inflight = g.fed - g.taken;
            if( clock < 0 ? inflight < 10 : ( t <= clock + GpuH264::LEAD_MS / 1000.0 && inflight < GpuH264::MAX_IN_FLIGHT ) )
                break;
            Sleep( 4 );
        }
        if( m_quit || m_flushing )
        {
            fg_free_packet( pkt );
            continue;
        }
        unsigned char* bytes = NULL;
        int size = 0;
        bool headers = g.needHeaders;
        bool ok = fg_packet_annexb( m_media, pkt, &bytes, &size, g.needHeaders ? 1 : 0 ) == 0;
        if( ok && g.needHeaders && size > 10 && bytes[6] == 0 && bytes[7] == 0 && bytes[8] == 0 && bytes[9] == 1 && ( bytes[10] & 31 ) == 7 )
            g.needHeaders = false;
        if( ok && !headers && ( t < 0 || ( m_reachingFloor && t < m_seekFloor - 0.02 ) ) && GpuDisposable( bytes, size ) )
        {
            ok = false;
            g.lastT = t;
        }
        if( ok )
        {
            std::vector<BYTE> chunk( bytes, bytes + size );
            EnterCriticalSection( &g.lock );
            g.chunks.push_back( std::vector<BYTE>() );
            g.chunks.back().swap( chunk );
            __int64 stamp = GpuStamp( t, g.flushGen );
            g.chunkStamps.push_back( stamp );
            g.stamps.insert( stamp );
            ++g.fed;
            g.lastT = t;
            LeaveCriticalSection( &g.lock );
            SetEvent( g.dataEvent );
        }
        fg_free_packet( pkt );
    }
}

void FFPlayer::GpuFlush()
{
    GpuState& g = *m_g;
    EnterCriticalSection( &g.lock );
    g.chunks.clear();
    g.chunkStamps.clear();
    g.stamps.clear();
    g.fed = g.taken = 0;
    g.needHeaders = true;
    g.firstPts = -1.0;
    g.ready = 0;
    g.flushTick = GetTickCount();
    g.skipped = 0;
    InterlockedIncrement( &g.flushGen );
    LeaveCriticalSection( &g.lock );
}

void FFPlayer::GpuClose()
{
    if( !m_g )
        return;
    GpuState& g = *m_g;
    GpuH264::Detach();
    for( int i = 0; i < g.qn; ++i )
        GpuH264::Release( g.q[i] );
    if( g.held )
        GpuH264::Release( g.cur );
    GpuH264::Close();
    if( g.renderThread && g.renderThread == GetCurrentThreadId() )
    {
        XSetThreadProcessor( GetCurrentThread(), g.oldCore );
        SetThreadPriority( GetCurrentThread(), g.oldPriority );
    }
    else if( g.renderThread )
        Log::Write( "GPU: closed from another thread, render thread stays on its core" );
    delete m_g;
    m_g = NULL;
}

void FFPlayer::GpuPump()
{
    GpuState& g = *m_g;
    if( !g.opened )
    {
        g.opened = true;
        Log::Write( "GPU: opening the decoder" );
        if( !GpuH264::Open( GpuRead, this, g.level ) )
            Log::Write( "GPU decoder could not start" );
        else
        {
            Log::Write( "GPU: decoder open" );
            g.oldCore = GetCurrentProcessorNumber();
            g.renderThread = GetCurrentThreadId();
            g.oldPriority = GetThreadPriority( GetCurrentThread() );
            XSetThreadProcessor( GetCurrentThread(), GpuH264::RENDER_CORE );
            SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
        }
    }
    GpuH264::Pump();
    GpuTake();
}

void FFPlayer::GpuTake()
{
    GpuState& g = *m_g;
    LONG gen = g.flushGen;
    if( gen != g.seenGen )
    {
        g.seenGen = gen;
        for( int i = 0; i < g.qn; ++i )
            GpuH264::Release( g.q[i] );
        g.qn = 0;
    }
    while( g.qn < GpuH264::QUEUE_FRAMES )
    {
        GpuH264::Frame f;
        if( !GpuH264::Take( f ) )
            break;
        EnterCriticalSection( &g.lock );
        std::multiset<__int64>::iterator it = g.stamps.find( f.stamp );
        if( it == g.stamps.end() )
        {
            LeaveCriticalSection( &g.lock );
            GpuH264::Release( f );
            continue;
        }
        while( g.stamps.begin() != it )
        {
            g.stamps.erase( g.stamps.begin() );
            ++g.taken;
        }
        g.stamps.erase( it );
        ++g.taken;
        LONG takenGen = g.flushGen;
        LeaveCriticalSection( &g.lock );
        double pts = GpuStampTime( f.stamp );
        bool preroll = pts < 0;
        if( g.taken == 1 )
            Log::Write( "GPU: first decoded frame (pts %.3f)", pts );
        InterlockedIncrement( &m_decoded );
        if( preroll )
        {
            GpuH264::Release( f );
            continue;
        }
        if( m_reachingFloor )
        {
            if( pts >= 0 && pts < m_seekFloor - 0.02 )
            {
                GpuH264::Release( f );
                ++g.skipped;
                continue;
            }
            InterlockedExchange( &m_reachingFloor, 0 );
            Log::Write( "GPU: seek target reached in %lu ms (%ld frames decoded on the way)", GetTickCount() - g.flushTick, g.skipped );
        }
        EnterCriticalSection( &g.lock );
        bool current = g.flushGen == takenGen;
        if( current )
        {
            if( g.firstPts < 0 )
                g.firstPts = pts;
            ++g.ready;
        }
        LeaveCriticalSection( &g.lock );
        if( !current )
        {
            GpuH264::Release( f );
            continue;
        }
        g.q[g.qn] = f;
        g.qPts[g.qn] = pts;
        ++g.qn;
    }
}

void FFPlayer::GpuSelect( double clock )
{
    GpuState& g = *m_g;
    static double s_refresh = 0;
    if( !s_refresh )
    {
        XVIDEO_MODE vm;
        XGetVideoMode( &vm );
        s_refresh = vm.RefreshRate > 20.0f ? 1.0 / vm.RefreshRate : 1001.0 / 60000.0;
        Log::Write( "GPU: refresh %.3f Hz", 1.0 / s_refresh );
    }
    LONG blank = g_swapBlank;
    if( clock >= 0 && !m_paused && blank != 0 )
    {
        if( g.vOn )
            g.vClock += ( blank - g.vBlank ) * s_refresh;
        g.vBlank = blank;
        double err = clock - g.vClock;
        if( !g.vOn || err > 0.05 || err < -0.05 )
        {
            g.vClock = clock;
            g.vBias = 0;
            g.vOn = true;
        }
        else
            g.vClock += err * 0.02;
        clock = g.vClock + g.vBias;
    }
    else
        g.vOn = false;
    while( g.qn > 0 )
    {
        if( clock < 0 ? m_haveFrame : g.qPts[0] > clock + 0.005 )
            break;
        double behind = g.frameDur < s_refresh * 1.5 ? 0.0 : 2 * g.frameDur;
        if( g.qn > 1 && clock >= 0 && g.qPts[1] < clock - behind )
        {
            GpuH264::Release( g.q[0] );
            g.PopFront();
            InterlockedIncrement( &m_dropped );
            continue;
        }
        if( g.held )
            GpuH264::Release( g.cur );
        g.cur = g.q[0];
        g.held = true;
        m_lastPts = g.qPts[0];
        g.PopFront();
        if( g.vOn && clock + 0.005 - m_lastPts < 0.002 )
        {
            g.vBias += 0.003;
            if( g.vBias > s_refresh * 0.5 )
                g.vBias -= s_refresh;
        }
        XGSetTextureHeader( g.cur.pitchY, g.cur.heightY, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g.tex[0], NULL, NULL );
        XGSetTextureHeader( g.cur.pitchC, g.cur.heightC, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g.tex[1], NULL, NULL );
        XGSetTextureHeader( g.cur.pitchC, g.cur.heightC, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g.tex[2], NULL, NULL );
        XGOffsetResourceAddress( &g.tex[0], (void*)g.cur.y );
        XGOffsetResourceAddress( &g.tex[1], (void*)g.cur.u );
        XGOffsetResourceAddress( &g.tex[2], (void*)g.cur.v );
        m_frameW = g.cur.showW;
        m_frameH = g.cur.showH;
        m_frameColorspace = FG_CS_UNKNOWN;
        m_frameFullRange = 0;
        if( !m_haveFrame )
            Log::Write( "GPU: first frame on screen" );
        m_haveFrame = true;
        m_lastShowTick = GetTickCount();
        InterlockedIncrement( &m_shown );
        if( g.vOn && m_lastPts + s_refresh * 0.75 < clock )
            ++s_lateFrames;
        break;
    }
}
