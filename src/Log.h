// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once

namespace Log
{
    void Open( const char* path );
    void Write( const char* fmt, ... );

    void Flush();

    void SetRemote( const char* ip, int port );
}
