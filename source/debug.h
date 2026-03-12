#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _DEBUG
inline void DbgTrace(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
}
#else
#define DbgTrace(...) ((void)0)
#endif
