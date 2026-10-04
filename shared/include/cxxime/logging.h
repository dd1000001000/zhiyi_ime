// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_LOGGING_H_
#define CXXIME_LOGGING_H_

#if defined(_DEBUG) || defined(CXXIME_ENABLE_TRACE_LOG)
#include <windows.h>  // OutputDebugStringW
#include <cstdio>
#define CXXIME_LOG(fmt, ...)                                                                       \
    do {                                                                                           \
        wchar_t _buf[512];                                                                        \
        _snwprintf_s(_buf, _countof(_buf), _TRUNCATE, L"[ZhiyiIME] " fmt L"\n", __VA_ARGS__);       \
        OutputDebugStringW(_buf);                                                                  \
    } while (0)
#else
#define CXXIME_LOG(fmt, ...) do {} while (0)
#endif

#endif // CXXIME_LOGGING_H_
