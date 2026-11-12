// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

class App
{
public:
    App();
    virtual ~App() {}

    void Run();

    IDirect3DDevice9*     m_pd3dDevice;
    D3DPRESENT_PARAMETERS m_d3dpp;

protected:
    virtual HRESULT Initialize() = 0;
    virtual HRESULT Update() = 0;
    virtual HRESULT Render() = 0;
};

int ScreenWidth();

D3DRECT SafeArea();

void FillGradient( DWORD top, DWORD bottom );

struct Pad
{
    WORD  wButtons;
    WORD  wPressedButtons;
    SHORT sThumbLX, sThumbLY;
    float fX1, fY1;
};
Pad* ReadPads();
