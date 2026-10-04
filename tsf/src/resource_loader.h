// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_RESOURCE_LOADER_H_
#define CXXIME_TSF_RESOURCE_LOADER_H_

#include <windows.h>
#include "cxxime_resource_ids.h"

namespace cxxime_tsf {

bool get_resource_dll_path(wchar_t* path, DWORD path_chars);
HICON load_resource_icon(UINT resource_id, int cx, int cy);

// Input method name in the Windows UI language: 知意输入法 for Chinese, Zhiyi IME otherwise.
const wchar_t* localized_display_name();
// True when the Windows UI language is Chinese.
bool ui_language_is_chinese();

}  // namespace cxxime_tsf

#endif  // CXXIME_TSF_RESOURCE_LOADER_H_
