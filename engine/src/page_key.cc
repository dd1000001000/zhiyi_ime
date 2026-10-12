// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/processor.h>

#include <windows.h>

namespace cxxime {

namespace {

char page_key_char(uint32_t keycode) {
    switch (keycode) {
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_4: return '[';
    case VK_OEM_6: return ']';
    default: return 0;
    }
}

}  // namespace

PageKey page_key(const KeyEvent& event, const Context& context) {
    if (event.keycode == VK_PRIOR) return PageKey::kPrevious;
    if (event.keycode == VK_NEXT) return PageKey::kNext;
    if (event.is_shift() || event.is_ctrl() || event.is_alt()) return PageKey::kNone;
    const char key = page_key_char(event.keycode);
    if (key == 0) return PageKey::kNone;
    if (key == context.page_up_key) return PageKey::kPrevious;
    if (key == context.page_down_key) return PageKey::kNext;
    return PageKey::kNone;
}

}  // namespace cxxime
