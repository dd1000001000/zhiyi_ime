// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TEXT_SERVICE_PROFILE_H_
#define CXXIME_TEXT_SERVICE_PROFILE_H_

#include <windows.h>

namespace cxxime {

inline constexpr CLSID kTextServiceClsid = {
    0x4eac2df0,
    0xf298,
    0x453e,
    {0xbd, 0x4a, 0xb4, 0xb3, 0xd3, 0x57, 0x97, 0x18},
};

inline constexpr GUID kTextServiceProfileGuid = {
    0x3ef70cbb,
    0xaa69,
    0x4cbc,
    {0xb4, 0x2f, 0xda, 0x5b, 0x90, 0xfc, 0x96, 0xf5},
};

inline constexpr LANGID kTextServiceLanguageId =
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

} // namespace cxxime

#endif // CXXIME_TEXT_SERVICE_PROFILE_H_
