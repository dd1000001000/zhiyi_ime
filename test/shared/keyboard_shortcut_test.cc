// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdint>

#include <windows.h>

#include <cxxime/keyboard_shortcut.h>

#include "support/testutil.h"

TEST(KeyboardShortcut, parses_and_formats_supported_keys) {
    struct TestCase {
        const char* value;
        const char* canonical;
        uint32_t modifiers;
        uint32_t virtual_key;
    };
    const TestCase test_cases[] = {
        {"f4", "F4", 0, VK_F4},
        {"ctrl + alt + c", "Ctrl+Alt+C",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierAlt,
         'C'},
        {"Ctrl+Shift+9", "Ctrl+Shift+9",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierShift,
         '9'},
        {"ctrl_shift_m", "Ctrl+Shift+M",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierShift,
         'M'},
        {"Ctrl+Alt+F11", "Ctrl+Alt+F11",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierAlt,
         VK_F11},
        {"Ctrl+Alt+Shift+Space", "Ctrl+Alt+Shift+Space",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierAlt | cxxime::kKeyModifierShift,
         VK_SPACE},
        {"Ctrl+/", "Ctrl+/", cxxime::kKeyModifierControl, VK_OEM_2},
        {"Ctrl+Shift+/", "Ctrl+Shift+/",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierShift,
         VK_OEM_2},
        {"Alt+;", "Alt+;", cxxime::kKeyModifierAlt, VK_OEM_1},
        {"Ctrl+Shift+=", "Ctrl+Shift+=",
         cxxime::kKeyModifierControl | cxxime::kKeyModifierShift,
         VK_OEM_PLUS},
    };
    for (const TestCase& test_case : test_cases) {
        cxxime::KeyboardShortcut shortcut;
        ASSERT_TRUE(cxxime::parse_keyboard_shortcut(test_case.value, &shortcut));
        ASSERT_EQ(shortcut.modifiers, test_case.modifiers);
        ASSERT_EQ(shortcut.virtual_key, test_case.virtual_key);
        ASSERT_TRUE(cxxime::keyboard_shortcut_string(shortcut) == test_case.canonical);
    }

    cxxime::KeyboardShortcut disabled;
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("disabled", &disabled));
    ASSERT_TRUE(!disabled.enabled());
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(disabled) == "disabled");
}

TEST(KeyboardShortcut, rejects_malformed_or_unsupported_keys) {
    const char* invalid_values[] = {
        "unknown",
        "Ctrl+Alt+Escape",
        "Ctrl+Alt+F12",
        "Ctrl+Alt+F13",
        "Ctrl+Ctrl+Alt+C",
        "Ctrl+Alt+C+",
    };
    for (const char* value : invalid_values) {
        cxxime::KeyboardShortcut shortcut;
        ASSERT_TRUE(!cxxime::parse_keyboard_shortcut(value, &shortcut));
    }
}

TEST(KeyboardShortcut, validators_apply_context_specific_rules) {
    cxxime::KeyboardShortcut shortcut;

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("F4", &shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Shift+F4", &shortcut));
    ASSERT_TRUE(!cxxime::is_valid_activate_ime_shortcut(shortcut));

    // Copy stays with the application.
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+C", &shortcut));
    ASSERT_TRUE(cxxime::is_common_app_shortcut(shortcut));
    ASSERT_TRUE(!cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));

    for (const char* common : {"Ctrl+V", "Ctrl+X", "Ctrl+Z", "Ctrl+Y", "Ctrl+A", "Ctrl+S",
                               "Ctrl+F", "Alt+F4", "Alt+Space"}) {
        ASSERT_TRUE(cxxime::parse_keyboard_shortcut(common, &shortcut));
        ASSERT_TRUE(cxxime::is_common_app_shortcut(shortcut));
        ASSERT_TRUE(!cxxime::is_valid_input_mode_shortcut(shortcut));
    }
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+Shift+C", &shortcut));
    ASSERT_TRUE(!cxxime::is_common_app_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+/", &shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Shift+/", &shortcut));
    ASSERT_TRUE(!cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(!cxxime::is_valid_activate_ime_shortcut(shortcut));

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Alt+;", &shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));

    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Ctrl+Alt+C", &shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));

    // Shift+Space switches full/half width; Shift with other keys types.
    ASSERT_TRUE(cxxime::parse_keyboard_shortcut("Shift+Space", &shortcut));
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(!cxxime::is_valid_activate_ime_shortcut(shortcut));

    shortcut = {};
    ASSERT_TRUE(cxxime::is_valid_input_mode_shortcut(shortcut));
    ASSERT_TRUE(cxxime::is_valid_activate_ime_shortcut(shortcut));
}

TEST(KeyboardShortcut, reads_win32_hotkeys_and_flags_common_program_shortcuts) {
    // ImmGetHotKey reports Ctrl+Space as MOD_CONTROL plus side flags (0xC000).
    const cxxime::KeyboardShortcut ctrl_space =
        cxxime::shortcut_from_win32_hotkey(MOD_CONTROL | 0xC000, VK_SPACE);
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(ctrl_space) == "Ctrl+Space");
    const cxxime::KeyboardShortcut ctrl_alt_g =
        cxxime::shortcut_from_win32_hotkey(MOD_CONTROL | MOD_ALT, 'G');
    ASSERT_TRUE(cxxime::keyboard_shortcut_string(ctrl_alt_g) == "Ctrl+Alt+G");

    cxxime::KeyboardShortcut shortcut;
    for (const char* common : {"Ctrl+T", "Ctrl+W", "Ctrl+N", "Ctrl+P", "Ctrl+R", "F5", "F1"}) {
        ASSERT_TRUE(cxxime::parse_keyboard_shortcut(common, &shortcut));
        ASSERT_TRUE(cxxime::is_common_program_shortcut(shortcut));
    }
    for (const char* other : {"Ctrl+Space", "Ctrl+Shift+T", "F8", "Alt+T"}) {
        ASSERT_TRUE(cxxime::parse_keyboard_shortcut(other, &shortcut));
        ASSERT_TRUE(!cxxime::is_common_program_shortcut(shortcut));
    }
}

TEST(KeyboardShortcut, converts_modifiers_for_register_hotkey) {
    const cxxime::KeyboardShortcut shortcut = {
        cxxime::kKeyModifierControl | cxxime::kKeyModifierAlt | cxxime::kKeyModifierShift,
        'C',
    };
    const uint32_t modifiers = cxxime::keyboard_shortcut_win32_modifiers(shortcut);
    ASSERT_TRUE((modifiers & MOD_CONTROL) != 0);
    ASSERT_TRUE((modifiers & MOD_ALT) != 0);
    ASSERT_TRUE((modifiers & MOD_SHIFT) != 0);
    ASSERT_TRUE((modifiers & MOD_NOREPEAT) != 0);
}

RUN_ALL_TESTS()
