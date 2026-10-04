// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Conflicts of a switch key on this user's machine, checked live (each machine differs):
// Windows input method hotkeys (ImmGetHotKey: the Chinese IME/non-IME toggle on Ctrl+Space,
// layout hotkeys, other IMEs' private hotkeys ...), which the IME takes over through its
// preserved keys, and global hotkeys of other programs (RegisterHotKey), which come first and
// make the switch key do nothing.
#ifndef CXXIME_SETTINGS_HOTKEY_CHECK_H_
#define CXXIME_SETTINGS_HOTKEY_CHECK_H_

#include <cxxime/keyboard_shortcut.h>

namespace cxxime {
namespace settings {

enum class SystemHotkey {
    kNone,
    kImeToggle,  // input method on/off (Chinese/English)
    kShape,      // full/half width
    kSymbol,     // Chinese/English punctuation
    kLayout,     // switch to a keyboard layout / input language
    kOtherIme,   // another input method's hotkey
};

SystemHotkey system_ime_hotkey(const KeyboardShortcut& shortcut);

// True when another program holds the combination as a global hotkey (probed by registering
// it for a moment).
bool taken_by_other_program(const KeyboardShortcut& shortcut);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_HOTKEY_CHECK_H_
