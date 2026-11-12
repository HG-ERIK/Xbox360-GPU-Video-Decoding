// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

#define FONT_LEFT       0x00000000
#define FONT_RIGHT      0x00000001
#define FONT_CENTER_X   0x00000002
#define FONT_TRUNCATED  0x00000008

class Font
{
public:
    Font();
    ~Font();

    static void Startup( IDirect3DDevice9* device );

    HRESULT Create( const char* path );

    void  Begin();
    void  End();

    void  DrawText( float x, float y, DWORD color, const wchar_t* text, DWORD flags = 0, float maxWidth = 0 );

    void  GetTextExtent( const wchar_t* text, float* width, float* height, BOOL firstLineOnly = FALSE ) const;
    float GetTextWidth( const wchar_t* text ) const;
    float GetFontHeight() const { return (float)m_lineAdvance; }

    void  SetScaleFactors( float x, float y );
    void  SetWindow( const D3DRECT& rc ) { m_window = rc; }
    void  SetWindow( LONG x1, LONG y1, LONG x2, LONG y2 );
    void  GetWindow( D3DRECT& rc ) const { rc = m_window; }

private:
    struct Glyph
    {
        WORD  x, y, width;
        SHORT offset;
        WORD  advance;
    };

    const Glyph& GlyphFor( wchar_t c ) const;
    void         Apply();
    void         Restore();

    IDirect3DTexture9* m_texture;
    Glyph*             m_glyphs;
    WORD*              m_translator;
    int                m_maxChar;
    int                m_cellHeight, m_lineAdvance, m_texW, m_texH;
    float              m_scaleX, m_scaleY;
    D3DRECT            m_window;
    int                m_beginCount;
};
