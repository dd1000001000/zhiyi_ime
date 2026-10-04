// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/keyboard_shortcut.h>

#include <cctype>

#include <windows.h>

namespace cxxime {
namespace {

struct NamedVirtualKey {
    const char* name;
    uint32_t virtual_key;
};

// Persist the unshifted key label; for example, '?' is represented as Shift+/.
constexpr NamedVirtualKey kOemVirtualKeys[] = {
    {";", VK_OEM_1},
    {"=", VK_OEM_PLUS},
    {",", VK_OEM_COMMA},
    {"-", VK_OEM_MINUS},
    {".", VK_OEM_PERIOD},
    {"/", VK_OEM_2},
    {"`", VK_OEM_3},
    {"[", VK_OEM_4},
    {"\\", VK_OEM_5},
    {"]", VK_OEM_6},
    {"'", VK_OEM_7},
};

bool is_supported_virtual_key(uint32_t virtual_key) {
    // Windows reserves F12 for debugger use, so shortcut handling stops at F11.
    if ((virtual_key >= 'A' && virtual_key <= 'Z') ||
        (virtual_key >= '0' && virtual_key <= '9') ||
        (virtual_key >= VK_F1 && virtual_key <= VK_F11) || virtual_key == VK_SPACE) {
        return true;
    }
    for (const auto& key : kOemVirtualKeys) {
        if (key.virtual_key == virtual_key) {
            return true;
        }
    }
    return false;
}

std::string normalize_token(const std::string& token) {
    std::string normalized;
    normalized.reserve(token.size());
    for (unsigned char ch : token) {
        if (ch == '_') {
            normalized.push_back('+');
        } else if (!std::isspace(ch)) {
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return normalized;
}

bool parse_virtual_key(const std::string& token, uint32_t* virtual_key) {
    if (!virtual_key) {
        return false;
    }
    if (token.size() == 1 &&
        ((token[0] >= 'A' && token[0] <= 'Z') ||
         (token[0] >= '0' && token[0] <= '9'))) {
        *virtual_key = static_cast<uint32_t>(token[0]);
        return true;
    }
    if (token == "SPACE") {
        *virtual_key = VK_SPACE;
        return true;
    }
    for (const auto& key : kOemVirtualKeys) {
        if (token == key.name) {
            *virtual_key = key.virtual_key;
            return true;
        }
    }
    if (token.size() >= 2 && token.size() <= 3 && token[0] == 'F') {
        uint32_t number = 0;
        for (size_t i = 1; i < token.size(); ++i) {
            if (token[i] < '0' || token[i] > '9') {
                return false;
            }
            number = number * 10 + static_cast<uint32_t>(token[i] - '0');
        }
        if (number >= 1 && number <= 11) {
            *virtual_key = VK_F1 + number - 1;
            return true;
        }
    }
    return false;
}

std::string virtual_key_name(uint32_t virtual_key) {
    if ((virtual_key >= 'A' && virtual_key <= 'Z') ||
        (virtual_key >= '0' && virtual_key <= '9')) {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key == VK_SPACE) {
        return "Space";
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F11) {
        return "F" + std::to_string(virtual_key - VK_F1 + 1);
    }
    for (const auto& key : kOemVirtualKeys) {
        if (key.virtual_key == virtual_key) {
            return key.name;
        }
    }
    return {};
}

} // namespace

std::string keyboard_shortcut_string(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled() || !is_valid_keyboard_shortcut(shortcut)) {
        return "disabled";
    }

    std::string value;
    if ((shortcut.modifiers & kKeyModifierControl) != 0) {
        value += "Ctrl+";
    }
    if ((shortcut.modifiers & kKeyModifierAlt) != 0) {
        value += "Alt+";
    }
    if ((shortcut.modifiers & kKeyModifierShift) != 0) {
        value += "Shift+";
    }
    value += virtual_key_name(shortcut.virtual_key);
    return value;
}

bool parse_keyboard_shortcut(const std::string& value, KeyboardShortcut* shortcut) {
    if (!shortcut) {
        return false;
    }
    const std::string normalized = normalize_token(value);
    if (normalized == "DISABLED") {
        *shortcut = {};
        return true;
    }

    KeyboardShortcut parsed;
    size_t start = 0;
    while (start < normalized.size()) {
        const size_t separator = normalized.find('+', start);
        const std::string token = normalized.substr(start, separator - start);
        if (token.empty()) {
            return false;
        }
        if (token == "CTRL") {
            if ((parsed.modifiers & kKeyModifierControl) != 0) {
                return false;
            }
            parsed.modifiers |= kKeyModifierControl;
        } else if (token == "ALT") {
            if ((parsed.modifiers & kKeyModifierAlt) != 0) {
                return false;
            }
            parsed.modifiers |= kKeyModifierAlt;
        } else if (token == "SHIFT") {
            if ((parsed.modifiers & kKeyModifierShift) != 0) {
                return false;
            }
            parsed.modifiers |= kKeyModifierShift;
        } else if (parsed.virtual_key != 0 || !parse_virtual_key(token, &parsed.virtual_key)) {
            return false;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
        if (start >= normalized.size()) {
            return false;
        }
    }
    if (!is_valid_keyboard_shortcut(parsed) || !parsed.enabled()) {
        return false;
    }
    *shortcut = parsed;
    return true;
}

bool is_valid_keyboard_shortcut(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) {
        return shortcut.modifiers == 0;
    }
    return (shortcut.modifiers & ~kShortcutModifierMask) == 0 &&
           is_supported_virtual_key(shortcut.virtual_key);
}

bool is_common_app_shortcut(const KeyboardShortcut& shortcut) {
    if (shortcut.modifiers == kKeyModifierControl) {
        switch (shortcut.virtual_key) {
        case 'A':
        case 'C':
        case 'F':
        case 'S':
        case 'V':
        case 'X':
        case 'Y':
        case 'Z':
            return true;
        default:
            return false;
        }
    }
    return shortcut.modifiers == kKeyModifierAlt &&
           (shortcut.virtual_key == VK_F4 || shortcut.virtual_key == VK_SPACE);
}

bool is_valid_input_mode_shortcut(const KeyboardShortcut& shortcut) {
    if (!is_valid_keyboard_shortcut(shortcut)) {
        return false;
    }
    if (!shortcut.enabled()) {
        return true;
    }
    if (is_common_app_shortcut(shortcut)) {
        return false;
    }
    if (shortcut.virtual_key >= VK_F1 && shortcut.virtual_key <= VK_F11) {
        return true;
    }
    if (shortcut.modifiers == kKeyModifierShift && shortcut.virtual_key == VK_SPACE) {
        return true;
    }
    return (shortcut.modifiers & (kKeyModifierControl | kKeyModifierAlt)) != 0;
}

bool is_valid_activate_ime_shortcut(const KeyboardShortcut& shortcut) {
    if (!is_valid_keyboard_shortcut(shortcut)) {
        return false;
    }
    if (!shortcut.enabled()) {
        return true;
    }
    if (shortcut.modifiers == 0 &&
        shortcut.virtual_key >= VK_F1 && shortcut.virtual_key <= VK_F11) {
        return true;
    }
    return (shortcut.modifiers & (kKeyModifierControl | kKeyModifierAlt)) != 0;
}

KeyboardShortcut shortcut_from_win32_hotkey(uint32_t win32_modifiers, uint32_t virtual_key) {
    KeyboardShortcut shortcut;
    shortcut.virtual_key = virtual_key;
    if ((win32_modifiers & MOD_ALT) != 0) shortcut.modifiers |= kKeyModifierAlt;
    if ((win32_modifiers & MOD_CONTROL) != 0) shortcut.modifiers |= kKeyModifierControl;
    if ((win32_modifiers & MOD_SHIFT) != 0) shortcut.modifiers |= kKeyModifierShift;
    return shortcut;
}

bool is_common_program_shortcut(const KeyboardShortcut& shortcut) {
    if (shortcut.modifiers == 0) {
        return shortcut.virtual_key >= VK_F1 && shortcut.virtual_key <= VK_F5;
    }
    if (shortcut.modifiers != kKeyModifierControl) {
        return false;
    }
    switch (shortcut.virtual_key) {
    case 'N':
    case 'O':
    case 'P':
    case 'R':
    case 'T':
    case 'W':
        return true;
    default:
        return false;
    }
}

uint32_t keyboard_shortcut_win32_modifiers(const KeyboardShortcut& shortcut) {
    uint32_t modifiers = MOD_NOREPEAT;
    if ((shortcut.modifiers & kKeyModifierShift) != 0) {
        modifiers |= MOD_SHIFT;
    }
    if ((shortcut.modifiers & kKeyModifierControl) != 0) {
        modifiers |= MOD_CONTROL;
    }
    if ((shortcut.modifiers & kKeyModifierAlt) != 0) {
        modifiers |= MOD_ALT;
    }
    return modifiers;
}

} // namespace cxxime
