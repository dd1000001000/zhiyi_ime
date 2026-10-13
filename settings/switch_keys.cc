// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "switch_keys.h"

#include <string>

#include <windows.h>

namespace cxxime {
namespace settings {

namespace {

// The switch_key action of a tapped modifier for each switch, in box order; "noop" is none.
constexpr const char* kTapActions[kSwitchKeyCount] = {"code", "toggle_style", "toggle_punct",
                                                      "toggle_shape", "toggle_voice"};

// A tapped modifier: its left/right switch_key names, the generic VK (either side) and the two
// side VKs.
struct TapKeys {
    const char* left;
    const char* right;
    uint32_t any_vk;
    uint32_t left_vk;
    uint32_t right_vk;
};
constexpr TapKeys kTapKeys[] = {
    {"Shift_L", "Shift_R", VK_SHIFT, VK_LSHIFT, VK_RSHIFT},
    {"Control_L", "Control_R", VK_CONTROL, VK_LCONTROL, VK_RCONTROL},
    {"Alt_L", "Alt_R", VK_MENU, VK_LMENU, VK_RMENU},
};

const TapKeys* tap_keys_of(uint32_t vk) {
    for (const TapKeys& keys : kTapKeys) {
        if (vk == keys.any_vk || vk == keys.left_vk || vk == keys.right_vk) return &keys;
    }
    return nullptr;
}

std::string tap_key_action(const Config& config, const char* key) {
    const auto found = config.ascii_switch_key.find(key);
    return found != config.ascii_switch_key.end() ? found->second : "noop";
}

bool is_toggle_action(const std::string& action) {
    for (int slot = 1; slot < kSwitchKeyCount; ++slot) {
        if (action == kTapActions[slot]) return true;
    }
    return false;
}

// Whether a switch_key action belongs to a switch: any action but noop and the four others taps
// the Chinese/English switch (code, set_ascii_mode, clear ...).
bool action_of_slot(const std::string& action, int slot) {
    if (slot == 0) return action != "noop" && !is_toggle_action(action);
    return action == kTapActions[slot];
}

}  // namespace

uint32_t tap_family(uint32_t vk) {
    const TapKeys* keys = tap_keys_of(vk);
    return keys ? keys->any_vk : 0;
}

TapSide tap_side(uint32_t vk) {
    const TapKeys* keys = tap_keys_of(vk);
    if (keys && vk == keys->left_vk) return TapSide::kLeft;
    if (keys && vk == keys->right_vk) return TapSide::kRight;
    return TapSide::kEither;
}

uint32_t tap_key_on_side(uint32_t family, TapSide side) {
    const TapKeys* keys = tap_keys_of(family);
    if (!keys) return 0;
    switch (side) {
    case TapSide::kLeft:
        return keys->left_vk;
    case TapSide::kRight:
        return keys->right_vk;
    case TapSide::kEither:
    default:
        return keys->any_vk;
    }
}

bool tap_keys_overlap(uint32_t left, uint32_t right) {
    return tap_family(left) != 0 && tap_family(left) == tap_family(right);
}

KeyChoice switch_key_choice(const Config& config, int slot) {
    const KeyboardShortcut* combos[] = {&config.ascii_toggle_shortcut,
                                        &config.english_style_shortcut,
                                        &config.punct_toggle_shortcut,
                                        &config.shape_toggle_shortcut,
                                        &config.voice_input_shortcut};
    if (combos[slot]->enabled()) return KeyChoice::of(*combos[slot]);
    // Each side on its own (a hand-written config may give the two sides to two switches).
    for (const TapKeys& keys : kTapKeys) {
        const bool left = action_of_slot(tap_key_action(config, keys.left), slot);
        const bool right = action_of_slot(tap_key_action(config, keys.right), slot);
        if (left && right) return KeyChoice::tap(keys.any_vk);
        if (left) return KeyChoice::tap(keys.left_vk);
        if (right) return KeyChoice::tap(keys.right_vk);
    }
    return KeyChoice::none();
}

void apply_switch_key_choices(Config& config, const KeyChoice (&choices)[kSwitchKeyCount]) {
    KeyboardShortcut* combos[] = {&config.ascii_toggle_shortcut, &config.english_style_shortcut,
                                  &config.punct_toggle_shortcut, &config.shape_toggle_shortcut,
                                  &config.voice_input_shortcut};
    for (const TapKeys& keys : kTapKeys) {
        config.ascii_switch_key[keys.left] = "noop";
        config.ascii_switch_key[keys.right] = "noop";
    }
    for (int slot = 0; slot < kSwitchKeyCount; ++slot) {
        const KeyChoice& choice = choices[slot];
        *combos[slot] = choice.kind == KeyChoice::Kind::kCombo ? choice.combo : KeyboardShortcut{};
        if (choice.kind != KeyChoice::Kind::kTap) continue;
        const TapKeys* keys = tap_keys_of(choice.tap_key);
        if (!keys) continue;
        // "code" for the Chinese/English switch: the keys typed so far are committed as letters,
        // then the mode switches.
        const char* action = kTapActions[slot];
        const TapSide side = tap_side(choice.tap_key);
        if (side != TapSide::kRight) config.ascii_switch_key[keys->left] = action;
        if (side != TapSide::kLeft) config.ascii_switch_key[keys->right] = action;
    }
}

}  // namespace settings
}  // namespace cxxime
