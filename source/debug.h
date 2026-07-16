#pragma once

#ifdef _DEBUG
void DbgTrace(const wchar_t* fmt, ...);
#else
#define DbgTrace(...) ((void)0)
#endif
