// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/key_event.h>

#include <windows.h>

namespace cxxime {

KeyEvent from_windows_key(uint32_t vk_code, uint32_t /*lparam*/, bool is_key_up) {
    KeyEvent event;
    event.keycode = vk_code;
    event.is_key_up = is_key_up;

    BYTE keyboard_state[256] = {};
    if (GetKeyboardState(keyboard_state)) {
        if (keyboard_state[VK_SHIFT] & 0x80)
            event.set_shift();
        if (keyboard_state[VK_CONTROL] & 0x80)
            event.set_ctrl();
        if (keyboard_state[VK_MENU] & 0x80)
            event.set_alt();
        if (keyboard_state[VK_CAPITAL] & 0x01)
            event.set_caps_lock();
    }

    return event;
}

uint32_t modifier_key_with_side(uint32_t vk_code, uint32_t lparam) {
    const bool extended = (lparam & (1u << 24)) != 0;
    const uint32_t scan = (lparam >> 16) & 0xFF;
    switch (vk_code) {
    case VK_SHIFT:
        return scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL:
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk_code;
    }
}

bool is_letter_key(uint32_t vk_code) {
    return vk_code >= 'A' && vk_code <= 'Z';
}

bool is_digit_key(uint32_t vk_code) {
    return vk_code >= '0' && vk_code <= '9';
}

std::optional<char> normalize_ascii_key(const KeyEvent& event) {
    if (event.is_key_up || event.is_ctrl() || event.is_alt()) {
        return std::nullopt;
    }

    const uint32_t vk = event.keycode;
    if (is_letter_key(vk)) {
        const bool upper = event.is_shift() != event.is_caps_lock();
        return upper ? static_cast<char>(vk) : static_cast<char>(vk - 'A' + 'a');
    }
    if (is_digit_key(vk)) {
        static constexpr char kShiftedDigits[] = ")!@#$%^&*(";
        return event.is_shift() ? kShiftedDigits[vk - '0'] : static_cast<char>(vk);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return static_cast<char>('0' + vk - VK_NUMPAD0);
    }

    switch (vk) {
    case VK_SPACE:
        return ' ';
    case VK_OEM_PERIOD:
        return event.is_shift() ? '>' : '.';
    case VK_OEM_COMMA:
        return event.is_shift() ? '<' : ',';
    case VK_OEM_1:
        return event.is_shift() ? ':' : ';';
    case VK_OEM_2:
        return event.is_shift() ? '?' : '/';
    case VK_OEM_3:
        return event.is_shift() ? '~' : '`';
    case VK_OEM_4:
        return event.is_shift() ? '{' : '[';
    case VK_OEM_5:
        return event.is_shift() ? '|' : '\\';
    case VK_OEM_6:
        return event.is_shift() ? '}' : ']';
    case VK_OEM_7:
        return event.is_shift() ? '"' : '\'';
    case VK_OEM_MINUS:
        return event.is_shift() ? '_' : '-';
    case VK_OEM_PLUS:
        return event.is_shift() ? '+' : '=';
    case VK_ADD:
        return '+';
    case VK_SUBTRACT:
        return '-';
    case VK_MULTIPLY:
        return '*';
    case VK_DIVIDE:
        return '/';
    case VK_DECIMAL:
        return '.';
    default:
        return std::nullopt;
    }
}

} // namespace cxxime
