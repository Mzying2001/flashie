#include "debug.h"

#ifdef _DEBUG

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

void DbgTrace(const wchar_t* fmt, ...)
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

#endif
