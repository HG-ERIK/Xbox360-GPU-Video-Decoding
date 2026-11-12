// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdarg.h>
#include "Log.h"
#include "Guard.h"

namespace
{
    char             g_path[MAX_PATH] = "";
    CRITICAL_SECTION g_lock;
    SOCKET           g_udp = INVALID_SOCKET;
    sockaddr_in      g_remote;

    char             g_pending[64 * 1024];
    size_t           g_pendingLen;
    CRITICAL_SECTION g_fileLock;
    HANDLE           g_wake;

    void WriteOut( const char* text, size_t len )
    {
        HANDLE h = CreateFile( g_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL );
        if( h != INVALID_HANDLE_VALUE )
        {
            DWORD wrote;
            SetFilePointer( h, 0, NULL, FILE_END );
            WriteFile( h, text, (DWORD)len, &wrote, NULL );
            FlushFileBuffers( h );
            CloseHandle( h );
        }
    }

    void WritePending( const char* extra = NULL, size_t extraLen = 0, bool wait = true )
    {
        static char s_out[sizeof( g_pending )];
        if( wait )
            EnterCriticalSection( &g_fileLock );
        else
        {
            int i = 0;
            while( !TryEnterCriticalSection( &g_fileLock ) )
            {
                if( ++i > 250 )
                    return;
                Sleep( 2 );
            }
        }
        if( wait )
            EnterCriticalSection( &g_lock );
        else
        {
            int i = 0;
            while( !TryEnterCriticalSection( &g_lock ) )
            {
                if( ++i > 250 )
                {
                    LeaveCriticalSection( &g_fileLock );
                    return;
                }
                Sleep( 2 );
            }
        }
        size_t len = g_pendingLen;
        memcpy( s_out, g_pending, len );
        g_pendingLen = 0;
        LeaveCriticalSection( &g_lock );
        if( len )
            WriteOut( s_out, len );
        if( extraLen )
            WriteOut( extra, extraLen );
        LeaveCriticalSection( &g_fileLock );
    }

    DWORD WINAPI Writer( LPVOID )
    {
        for( ;; )
        {
            WaitForSingleObject( g_wake, INFINITE );
            WritePending();
        }
    }
}

void Log::Open( const char* path )
{
    InitializeCriticalSection( &g_lock );
    InitializeCriticalSection( &g_fileLock );
    strcpy_s( g_path, path );
    FILE* f = fopen( g_path, "w" );
    if( f )
        fclose( f );
    g_wake = CreateEvent( NULL, FALSE, FALSE, NULL );
    HANDLE t = Guard_CreateThread( 0, Writer, NULL, CREATE_SUSPENDED );
    if( t )
    {
        XSetThreadProcessor( t, 2 );
        ResumeThread( t );
        CloseHandle( t );
    }
    else if( g_wake )
    {
        CloseHandle( g_wake );
        g_wake = NULL;
    }
}

void Log::Flush()
{
    if( g_path[0] && g_wake )
        WritePending( NULL, 0, false );
}

void Log::Write( const char* fmt, ... )
{
    char msg[1024];
    va_list args;
    va_start( args, fmt );
    vsnprintf_s( msg, sizeof( msg ), _TRUNCATE, fmt, args );
    va_end( args );

    char line[1100];
    sprintf_s( line, "[%8.3f] %s\n", GetTickCount() / 1000.0, msg );

    if( g_udp != INVALID_SOCKET )
        sendto( g_udp, line, (int)strlen( line ), 0, (sockaddr*)&g_remote, sizeof( g_remote ) );
    if( g_path[0] )
    {
        EnterCriticalSection( &g_lock );
        size_t len = strlen( line );
        bool full = g_pendingLen + len > sizeof( g_pending );
        if( !full )
        {
            memcpy( g_pending + g_pendingLen, line, len );
            g_pendingLen += len;
        }
        LeaveCriticalSection( &g_lock );
        if( full || !g_wake )
            WritePending( full ? line : NULL, full ? len : 0 );
        else
            SetEvent( g_wake );
        return;
        LeaveCriticalSection( &g_lock );
    }
}

void Log::SetRemote( const char* ip, int port )
{
    SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if( s == INVALID_SOCKET )
        return;
    BOOL yes = TRUE;
    setsockopt( s, SOL_SOCKET, 0x5801, (const char*)&yes, sizeof( yes ) );
    ZeroMemory( &g_remote, sizeof( g_remote ) );
    g_remote.sin_family = AF_INET;
    g_remote.sin_port = htons( (u_short)port );
    g_remote.sin_addr.s_addr = inet_addr( ip );
    g_udp = s;
    Write( "Remote log to %s:%d", ip, port );
}
