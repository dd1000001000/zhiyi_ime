// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_KEY_EVENT_H_
#define CXXIME_KEY_EVENT_H_

#include <cstdint>
#include <optional>

namespace cxxime {

constexpr uint32_t kKeyModifierShift = 0x01;
constexpr uint32_t kKeyModifierControl = 0x02;
constexpr uint32_t kKeyModifierAlt = 0x04;
constexpr uint32_t kKeyModifierCapsLock = 0x08;
constexpr uint32_t kShortcutModifierMask =
    kKeyModifierShift | kKeyModifierControl | kKeyModifierAlt;

// Not keys: the voice key as the text service reads it (docs/key-gestures.md), sent as key
// codes Windows leaves unassigned. Press starts recording (or stops it, as the voice key did);
// release ends a long press (stops and recognizes); cancel drops the recording without a word.
constexpr uint32_t kVoiceKeyPress = 0x97;
constexpr uint32_t kVoiceKeyRelease = 0x98;
constexpr uint32_t kVoiceKeyCancel = 0x99;

struct KeyEvent {
    uint32_t keycode = 0;
    uint32_t modifiers = 0;
    bool is_key_up = false;

    bool is_shift() const { return (modifiers & kKeyModifierShift) != 0; }
    bool is_ctrl() const { return (modifiers & kKeyModifierControl) != 0; }
    bool is_alt() const { return (modifiers & kKeyModifierAlt) != 0; }
    bool is_caps_lock() const { return (modifiers & kKeyModifierCapsLock) != 0; }

    void set_shift() { modifiers |= kKeyModifierShift; }
    void set_ctrl() { modifiers |= kKeyModifierControl; }
    void set_alt() { modifiers |= kKeyModifierAlt; }
    void set_caps_lock() { modifiers |= kKeyModifierCapsLock; }
};

// Convert Windows WPARAM/LPARAM to KeyEvent
KeyEvent from_windows_key(uint32_t vk_code, uint32_t lparam, bool is_key_up);

// Check if a key is a letter (a-z)
bool is_letter_key(uint32_t vk_code);

// Check if a key is a digit (0-9)
bool is_digit_key(uint32_t vk_code);

// Map a key event to its US QWERTY printable ASCII character. Ctrl/Alt combinations and
// key-up events are application commands, not text input.
std::optional<char> normalize_ascii_key(const KeyEvent& event);

} // namespace cxxime

#endif // CXXIME_KEY_EVENT_H_
