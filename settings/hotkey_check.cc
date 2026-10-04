// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "hotkey_check.h"

#include <windows.h>
#include <imm.h>

// Exported by imm32 (documented), but imm.h only declares it for IME developers (immdev.h).
extern "C" BOOL WINAPI ImmGetHotKey(DWORD hotkey_id, LPUINT modifiers, LPUINT virtual_key,
                                    HKL* layout);

namespace cxxime {
namespace settings {

namespace {

// IMM hotkey ids (imm.h): Chinese simplified, Chinese traditional, Japanese, Korean, direct
// switches to an input language, and private hotkeys of input methods.
struct HotkeyRange {
    DWORD first;
    DWORD last;
};

SystemHotkey kind_of(DWORD id) {
    switch (id) {
    case IME_CHOTKEY_IME_NONIME_TOGGLE:
    case IME_THOTKEY_IME_NONIME_TOGGLE:
    case IME_JHOTKEY_CLOSE_OPEN:
        return SystemHotkey::kImeToggle;
    case IME_CHOTKEY_SHAPE_TOGGLE:
    case IME_THOTKEY_SHAPE_TOGGLE:
    case IME_KHOTKEY_SHAPE_TOGGLE:
        return SystemHotkey::kShape;
    case IME_CHOTKEY_SYMBOL_TOGGLE:
    case IME_THOTKEY_SYMBOL_TOGGLE:
        return SystemHotkey::kSymbol;
    default:
        break;
    }
    if (id >= IME_HOTKEY_DSWITCH_FIRST && id <= IME_HOTKEY_DSWITCH_LAST) {
        return SystemHotkey::kLayout;
    }
    return SystemHotkey::kOtherIme;
}

}  // namespace

SystemHotkey system_ime_hotkey(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) return SystemHotkey::kNone;
    const HotkeyRange ranges[] = {
        {0x10, 0x2F},  // Chinese simplified (IME_CHOTKEY_*)
        {0x30, 0x4F},  // Japanese (IME_JHOTKEY_*)
        {0x50, 0x6F},  // Korean (IME_KHOTKEY_*)
        {0x70, 0x8F},  // Chinese traditional (IME_THOTKEY_*)
        {IME_HOTKEY_DSWITCH_FIRST, IME_HOTKEY_DSWITCH_LAST},
        {IME_HOTKEY_PRIVATE_FIRST, IME_HOTKEY_PRIVATE_LAST},
    };
    for (const HotkeyRange& range : ranges) {
        for (DWORD id = range.first; id <= range.last; ++id) {
            UINT modifiers = 0, vk = 0;
            HKL layout = nullptr;
            if (!ImmGetHotKey(id, &modifiers, &vk, &layout) || vk == 0 ||
                (modifiers & MOD_IGNORE_ALL_MODIFIER) != 0) {
                continue;
            }
            if (shortcut_from_win32_hotkey(modifiers, vk) == shortcut) {
                return kind_of(id);
            }
        }
    }
    return SystemHotkey::kNone;
}

bool taken_by_other_program(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) return false;
    constexpr int kProbeId = 0x5A49;  // any id: the hotkey is released right away
    if (RegisterHotKey(nullptr, kProbeId, keyboard_shortcut_win32_modifiers(shortcut),
                       shortcut.virtual_key)) {
        UnregisterHotKey(nullptr, kProbeId);
        return false;
    }
    return GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
}

}  // namespace settings
}  // namespace cxxime
