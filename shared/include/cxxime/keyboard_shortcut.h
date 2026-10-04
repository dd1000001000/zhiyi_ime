// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_KEYBOARD_SHORTCUT_H_
#define CXXIME_KEYBOARD_SHORTCUT_H_

#include <cstdint>
#include <string>

#include <cxxime/key_event.h>

namespace cxxime {

struct KeyboardShortcut {
    uint32_t modifiers = 0;
    uint32_t virtual_key = 0;

    constexpr bool enabled() const { return virtual_key != 0; }
    constexpr bool matches(const KeyEvent& event) const {
        return enabled() && event.keycode == virtual_key &&
               (event.modifiers & kShortcutModifierMask) == modifiers;
    }
    constexpr bool operator==(const KeyboardShortcut& other) const {
        return modifiers == other.modifiers && virtual_key == other.virtual_key;
    }
    constexpr bool operator!=(const KeyboardShortcut& other) const { return !(*this == other); }
};

std::string keyboard_shortcut_string(const KeyboardShortcut& shortcut);
bool parse_keyboard_shortcut(const std::string& value, KeyboardShortcut* shortcut);
bool is_valid_keyboard_shortcut(const KeyboardShortcut& shortcut);
// A switch key (Chinese/English, style, punctuation, full/half width): F1-F11, Shift+Space, or
// Ctrl/Alt (optionally with Shift) with a letter, digit, punctuation key or Space; never one of
// the common application shortcuts below.
bool is_valid_input_mode_shortcut(const KeyboardShortcut& shortcut);
// Copy, paste, undo, save, find ... and Alt+F4 / Alt+Space, which a switch key would take away
// from applications.
bool is_common_app_shortcut(const KeyboardShortcut& shortcut);
bool is_valid_activate_ime_shortcut(const KeyboardShortcut& shortcut);
uint32_t keyboard_shortcut_win32_modifiers(const KeyboardShortcut& shortcut);

} // namespace cxxime

#endif // CXXIME_KEYBOARD_SHORTCUT_H_
