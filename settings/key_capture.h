// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Key capture box for the switch keys: left click, then press the key (a modifier tapped alone,
// F1-F11, or a combination such as Ctrl+Shift+E); right click clears it. A tapped modifier is
// taken as either side; the arrow at the right end of the box (or Alt+Down) picks one side.
// Sends WM_COMMAND with kKeyCaptureChanged to its parent when the key changes.
#ifndef CXXIME_SETTINGS_KEY_CAPTURE_H_
#define CXXIME_SETTINGS_KEY_CAPTURE_H_

#include <cstdint>
#include <functional>
#include <string>

#include <windows.h>

#include <cxxime/keyboard_shortcut.h>

#include "switch_keys.h"

namespace cxxime {
namespace settings {

constexpr WORD kKeyCaptureChanged = 0x4B43;  // WM_COMMAND notification code

// `allow_tap`: a modifier tapped alone (Shift, Ctrl, Alt) is accepted; otherwise only combinations.
HWND create_key_capture(int id, int x, int y, int width, int height, HWND parent, bool allow_tap);
void key_capture_set(HWND control, const KeyChoice& choice);
KeyChoice key_capture_get(HWND control);
// Checked before a pressed key (or a side picked from the arrow) is taken: a non-empty message
// (the key is used by another box) keeps the previous key and shows the message for a moment.
using KeyCaptureCheck = std::function<std::wstring(const KeyChoice&)>;
void key_capture_set_check(HWND control, KeyCaptureCheck check);

bool same_key_choice(const KeyChoice& left, const KeyChoice& right);

// A note after the key: gray information, or a warning on a yellow box.
enum class KeyNote { kNone, kInfo, kWarning };
void key_capture_set_note(HWND control, KeyNote kind, const std::wstring& text);

// "Shift", "右 Ctrl", "Ctrl + Space"; `none` for no key.
std::wstring key_choice_text(const KeyChoice& choice, const wchar_t* none);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_KEY_CAPTURE_H_
