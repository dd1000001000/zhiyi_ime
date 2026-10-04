// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Key capture box for the switch keys: left click, then press the key (a modifier tapped alone,
// or a combination such as Ctrl+Shift+E); right click clears it. Sends WM_COMMAND with
// kKeyCaptureChanged to its parent when the key changes.
#ifndef CXXIME_SETTINGS_KEY_CAPTURE_H_
#define CXXIME_SETTINGS_KEY_CAPTURE_H_

#include <cstdint>
#include <string>

#include <windows.h>

#include <cxxime/keyboard_shortcut.h>

namespace cxxime {
namespace settings {

struct KeyChoice {
    enum class Kind { kNone, kTap, kCombo };
    Kind kind = Kind::kNone;
    uint32_t tap_key = 0;     // VK_SHIFT or VK_CONTROL, tapped alone (either side)
    KeyboardShortcut combo;   // kCombo

    static KeyChoice none() { return {}; }
    static KeyChoice tap(uint32_t key) { return {Kind::kTap, key, {}}; }
    static KeyChoice of(const KeyboardShortcut& shortcut) {
        return shortcut.enabled() ? KeyChoice{Kind::kCombo, 0, shortcut} : KeyChoice{};
    }
};

constexpr WORD kKeyCaptureChanged = 0x4B43;  // WM_COMMAND notification code

// `allow_tap`: a modifier tapped alone (Shift, Ctrl) is accepted; otherwise only combinations.
HWND create_key_capture(int id, int x, int y, int width, int height, HWND parent, bool allow_tap);
void key_capture_set(HWND control, const KeyChoice& choice);
KeyChoice key_capture_get(HWND control);

// "Shift", "Ctrl + Space"; `none` for no key.
std::wstring key_choice_text(const KeyChoice& choice, const wchar_t* none);

// Combinations the IME already uses (Ctrl+. punctuation, Shift+Space full/half width).
bool is_reserved_shortcut(const KeyboardShortcut& shortcut);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_KEY_CAPTURE_H_
