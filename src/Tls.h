// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <string>

namespace Tls
{
    bool Startup( std::string& error );

    class Connection
    {
    public:
        Connection();
        ~Connection();

        bool Handshake( unsigned int socket, const std::string& host, std::string& error );
        int  Send( const char* data, int len );
        int  Recv( char* data, int len );
        void Close();

    private:
        struct Impl;
        Impl* m_impl;

        Connection( const Connection& );
        Connection& operator=( const Connection& );
    };

    std::string ErrorString( int mbedtlsError );
}
