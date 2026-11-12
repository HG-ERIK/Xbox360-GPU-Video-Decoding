// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#ifndef GUARD_H
#define GUARD_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

void   Guard_Start( void );

void   Guard_Beat( void );

int    Guard_Filter( unsigned long code, EXCEPTION_POINTERS* info, const char* where );

HANDLE Guard_CreateThread( SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags );

#ifdef __cplusplus
}
#endif

#endif
