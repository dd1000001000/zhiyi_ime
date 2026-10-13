// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The five switch keys of Settings > Keys (中英切换, 输入方式切换, 中英文标点, 全角/半角, 语音输入)
// and how they are stored: a combination in shortcuts.*, or a modifier tapped alone in
// ascii_composer.switch_key (Shift_L / Shift_R / Control_L / Control_R / Alt_L / Alt_R).
#ifndef CXXIME_SETTINGS_SWITCH_KEYS_H_
#define CXXIME_SETTINGS_SWITCH_KEYS_H_

#include <cstdint>

#include <cxxime/config.h>
#include <cxxime/keyboard_shortcut.h>

namespace cxxime {
namespace settings {

struct KeyChoice {
    enum class Kind { kNone, kTap, kCombo };
    Kind kind = Kind::kNone;
    // A modifier tapped alone: the generic VK_SHIFT / VK_CONTROL / VK_MENU for either side, or the
    // specific VK_LSHIFT / VK_RSHIFT / VK_LCONTROL / VK_RCONTROL / VK_LMENU / VK_RMENU for one side.
    uint32_t tap_key = 0;
    KeyboardShortcut combo;   // kCombo

    static KeyChoice none() { return {}; }
    static KeyChoice tap(uint32_t key) { return {Kind::kTap, key, {}}; }
    static KeyChoice of(const KeyboardShortcut& shortcut) {
        return shortcut.enabled() ? KeyChoice{Kind::kCombo, 0, shortcut} : KeyChoice{};
    }
};

// A tapped modifier: which one (the generic VK_SHIFT / VK_CONTROL / VK_MENU, 0 for any other key)
// and which side it stands for.
enum class TapSide { kEither, kLeft, kRight };
uint32_t tap_family(uint32_t vk);
TapSide tap_side(uint32_t vk);
// The tap key of a family on a side: (VK_CONTROL, kRight) -> VK_RCONTROL; 0 for no family.
uint32_t tap_key_on_side(uint32_t family, TapSide side);
// Two tapped modifiers clash when they share a family and either is "either side" or both are the
// same side: Ctrl and left Ctrl clash, left Ctrl and right Ctrl do not.
bool tap_keys_overlap(uint32_t left, uint32_t right);

constexpr int kSwitchKeyCount = 5;
// The key of a switch (box order) as the config stores it: its combination, else a tapped
// modifier (one side when only that side has the switch's action), else none.
KeyChoice switch_key_choice(const Config& config, int slot);
// The five boxes (box order) into the config.
void apply_switch_key_choices(Config& config, const KeyChoice (&choices)[kSwitchKeyCount]);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_SWITCH_KEYS_H_
