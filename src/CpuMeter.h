// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <xtl.h>

class CpuMeter
{
public:
    enum { THREADS = 6 };

    CpuMeter();
    ~CpuMeter();

    void  Start();
    void  Stop();
    bool  Running() const { return m_running != 0; }

    float Busy( int i ) const { return m_busy[i]; }

private:
    static DWORD WINAPI Spin( LPVOID p );

    struct Slot
    {
        CpuMeter* owner;
        int       index;
    };

    Slot          m_slots[THREADS];
    HANDLE        m_threads[THREADS];
    volatile LONG m_running;
    volatile float m_busy[THREADS];
};
