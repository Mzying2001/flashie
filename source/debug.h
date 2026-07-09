#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _DEBUG
inline void DbgTrace(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = _vscwprintf(fmt, ap);
    va_end(ap);
    if (len <= 0) return;

    va_start(ap, fmt);
    wchar_t* buf = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (buf) {
        _vsnwprintf_s(buf, len + 1, _TRUNCATE, fmt, ap);
        buf[len] = L'\0';
        OutputDebugStringW(buf);
        free(buf);
    }
    va_end(ap);
}
#else
#define DbgTrace(...) ((void)0)
#endif
