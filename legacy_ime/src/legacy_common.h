// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_LEGACY_IME_LEGACY_COMMON_H_
#define CXXIME_LEGACY_IME_LEGACY_COMMON_H_

#include <windows.h>
#include <imm.h>
#include <immdev.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <cxxime/ipc_protocol.h>

namespace cxxime_legacy {

inline constexpr wchar_t kUiClassName[] = L"ZhiyiImeUI";
inline constexpr size_t kMaxCompositionChars = 255;
inline constexpr DWORD kCandidateListIndex = 0;
inline constexpr DWORD kCandidateListMask = 1U << kCandidateListIndex;
inline constexpr DWORD kCompositionFlags =
    GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS | GCS_DELTASTART;

struct CompositionBuffer {
    COMPOSITIONSTRING cs;
    BYTE comp_attr[kMaxCompositionChars + 1];
    DWORD comp_clause[2];
    WCHAR comp_str[kMaxCompositionChars + 1];
    DWORD result_clause[2];
    WCHAR result_str[kMaxCompositionChars + 1];
};

std::wstring utf8_to_wide(const char* text);
std::wstring truncate_text(std::wstring text);
bool launch_server();
bool launch_settings(HWND parent);
bool resize_imcc(HIMCC& handle, DWORD bytes);
void fill_composition_string(COMPOSITIONSTRING& cs,
                             const std::wstring& comp,
                             const std::wstring& result);
uint32_t modifiers_from_key_state(const BYTE* key_state);
bool is_status_key(UINT key_code);
bool is_key_up_from_key_data(LPARAM key_data);
bool is_ime_ui_message(UINT msg);
bool should_eat_response(const cxxime::IPCResponse& response,
                         UINT key_code,
                         bool was_composing);
DWORD align4(DWORD value);

} // namespace cxxime_legacy

#endif // CXXIME_LEGACY_IME_LEGACY_COMMON_H_
