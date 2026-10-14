// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TEXT_SERVICE_PROFILE_H_
#define CXXIME_TEXT_SERVICE_PROFILE_H_

#include <windows.h>

namespace cxxime {

// Zhiyu IME is its own input identity: it coexists with upstream Zhiyi IME on the same machine
// (separate CLSID, profile, data directory, registry key, mutexes and window classes).
inline constexpr CLSID kTextServiceClsid = {
    0x94f5f388,
    0x9732,
    0x439e,
    {0xa7, 0x3f, 0xeb, 0xc4, 0xe6, 0xf4, 0xa6, 0x4b},
};

inline constexpr GUID kTextServiceProfileGuid = {
    0xa549be94,
    0x7073,
    0x440d,
    {0xb2, 0x93, 0xac, 0xf8, 0x82, 0x0a, 0x52, 0x7b},
};

inline constexpr LANGID kTextServiceLanguageId =
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

} // namespace cxxime

#endif // CXXIME_TEXT_SERVICE_PROFILE_H_
