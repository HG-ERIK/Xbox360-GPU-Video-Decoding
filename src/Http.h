// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <string>

namespace Http
{
    struct Response
    {
        int         status;
        std::string body;
        std::string error;

        Response() : status( 0 ) {}
    };

    struct Url
    {
        bool        https;
        std::string host;
        int         port;
        std::string path;

        Url() : https( false ), port( 80 ) {}
        bool Parse( const std::string& url );
    };

    bool Startup( std::string& error, std::string& localIp );

    Response Request( const char* method, const std::string& url, const std::string& extraHeaders,
                      const std::string& body = std::string(), unsigned long timeoutMs = 10000 );

    inline Response Get( const std::string& url, const std::string& extraHeaders, unsigned long timeoutMs = 10000 )
    {
        return Request( "GET", url, extraHeaders, std::string(), timeoutMs );
    }

    std::string Escape( const std::string& s );

    class Stream
    {
    public:
        Stream();
        ~Stream();

        bool Open( const std::string& url, const std::string& extraHeaders, __int64 offset,
                   std::string& error, int redirects = 0 );
        int  Read( char* buf, int size );
        void Close();

        void Abort();

        int     Status() const     { return m_status; }
        __int64 TotalSize() const  { return m_totalSize; }

    private:
        int  RawRead( char* buf, int size );
        bool ReadLine( std::string& line );

        unsigned int     m_socket;
        void*            m_tls;
        std::string      m_pending;
        size_t           m_pendingPos;
        int              m_status;
        __int64          m_totalSize;
        __int64          m_remaining;
        bool             m_chunked;
        __int64          m_chunkLeft;
        bool             m_eof;

        Stream( const Stream& );
        Stream& operator=( const Stream& );
    };
}
