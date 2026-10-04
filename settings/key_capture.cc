// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "key_capture.h"

#include <algorithm>
#include <cwchar>

#include <cxxime/key_event.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

constexpr wchar_t kClassName[] = L"ZhiyiKeyCapture";

struct State {
    KeyChoice choice;
    bool allow_tap = false;
    bool capturing = false;
    bool hover = false;
    const wchar_t* message = nullptr;  // why the last key was not taken (while capturing)
    // The key presses of one capture: modifiers seen, and whether another key came with them.
    uint32_t modifiers_seen = 0;
    bool other_key_seen = false;
};

State* state_of(HWND window) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

uint32_t modifier_bit(UINT vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return kKeyModifierShift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return kKeyModifierControl;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return kKeyModifierAlt;
    default:
        return 0;
    }
}

bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

uint32_t modifiers_down() {
    uint32_t modifiers = 0;
    if (key_down(VK_SHIFT)) modifiers |= kKeyModifierShift;
    if (key_down(VK_CONTROL)) modifiers |= kKeyModifierControl;
    if (key_down(VK_MENU)) modifiers |= kKeyModifierAlt;
    return modifiers;
}

void notify_changed(HWND window) {
    SendMessageW(GetParent(window), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window), kKeyCaptureChanged),
                 reinterpret_cast<LPARAM>(window));
}

void start_capture(HWND window, State* state) {
    state->capturing = true;
    state->message = nullptr;
    state->modifiers_seen = 0;
    state->other_key_seen = false;
    InvalidateRect(window, nullptr, TRUE);
}

void stop_capture(HWND window, State* state) {
    state->capturing = false;
    state->message = nullptr;
    InvalidateRect(window, nullptr, TRUE);
}

void accept(HWND window, State* state, const KeyChoice& choice) {
    state->choice = choice;
    stop_capture(window, state);
    notify_changed(window);
}

void reject(HWND window, State* state, const wchar_t* message) {
    state->message = message;
    InvalidateRect(window, nullptr, TRUE);
}

void on_key_down(HWND window, State* state, UINT vk, LPARAM lparam) {
    if ((lparam & (1 << 30)) != 0) return;  // auto-repeat
    if (const uint32_t bit = modifier_bit(vk)) {
        state->modifiers_seen |= bit;
        return;
    }
    if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK ||
        vk == VK_SCROLL) {
        reject(window, state, tr("keys.unsupported"));
        return;
    }
    state->other_key_seen = true;
    const KeyboardShortcut shortcut = {modifiers_down(), vk};
    if (vk == VK_ESCAPE && shortcut.modifiers == 0) {
        stop_capture(window, state);  // cancel, keep the key
        return;
    }
    if (!is_valid_input_mode_shortcut(shortcut)) {
        reject(window, state, is_valid_keyboard_shortcut(shortcut) ? tr("keys.need_combo")
                                                                   : tr("keys.unsupported"));
        return;
    }
    if (is_reserved_shortcut(shortcut)) {
        reject(window, state, tr("keys.reserved"));
        return;
    }
    accept(window, state, KeyChoice::of(shortcut));
}

void on_key_up(HWND window, State* state, UINT vk) {
    const uint32_t bit = modifier_bit(vk);
    if (bit == 0) return;
    // A modifier pressed and released alone.
    if (!state->other_key_seen && state->modifiers_seen == bit) {
        if (state->allow_tap && (bit == kKeyModifierShift || bit == kKeyModifierControl)) {
            accept(window, state, KeyChoice::tap(bit == kKeyModifierShift ? VK_SHIFT : VK_CONTROL));
            return;
        }
        reject(window, state, state->allow_tap ? tr("keys.tap_only") : tr("keys.need_combo"));
    }
    if (modifiers_down() == 0) {
        state->modifiers_seen = 0;
        state->other_key_seen = false;
    }
}

void paint(HWND window, State* state) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);
    RECT rect;
    GetClientRect(window, &rect);
    FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));

    const bool focused = GetFocus() == window;
    const COLORREF border = state->capturing ? RGB(0, 103, 192)
                            : state->hover    ? RGB(120, 120, 120)
                                              : RGB(170, 170, 170);
    const int thickness = state->capturing ? (std::max)(2, S(2)) : 1;
    HBRUSH border_brush = CreateSolidBrush(border);
    for (int i = 0; i < thickness; ++i) {
        RECT frame = {rect.left + i, rect.top + i, rect.right - i, rect.bottom - i};
        FrameRect(dc, &frame, border_brush);
    }
    DeleteObject(border_brush);

    std::wstring text;
    COLORREF color = GetSysColor(COLOR_WINDOWTEXT);
    if (state->capturing) {
        text = state->message ? state->message : tr("keys.press");
        color = state->message ? RGB(196, 43, 28) : RGB(0, 103, 192);
    } else {
        text = key_choice_text(state->choice, tr("keys.none"));
        if (state->choice.kind == KeyChoice::Kind::kNone) color = RGB(130, 130, 130);
    }
    HGDIOBJ old_font = SelectObject(dc, get_font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT text_rect = {rect.left + S(8), rect.top, rect.right - S(6), rect.bottom};
    DrawTextW(dc, text.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old_font);
    if (focused && !state->capturing) {
        RECT focus = {rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3};
        DrawFocusRect(dc, &focus);
    }
    EndPaint(window, &ps);
}

LRESULT CALLBACK key_capture_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = state_of(window);
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* initial = new State();
        initial->allow_tap = create->lpCreateParams != nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));
        break;
    }
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_GETDLGCODE:
        // While capturing every key (Tab, Enter, Esc, arrows) belongs to the box.
        if (state && state->capturing) return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTTAB;
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        if (state) start_capture(window, state);
        return 0;
    case WM_RBUTTONUP:
        if (state) {
            SetFocus(window);
            accept(window, state, KeyChoice::none());
        }
        return 0;
    case WM_CONTEXTMENU:
        return 0;  // right click clears instead
    case WM_MOUSEMOVE:
        if (state && !state->hover) {
            state->hover = true;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state) {
            state->hover = false;
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_SETFOCUS:
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_KILLFOCUS:
        if (state && state->capturing) {
            stop_capture(window, state);  // cancelled
        } else {
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!state) break;
        if (state->capturing) {
            on_key_down(window, state, static_cast<UINT>(wparam), lparam);
            return 0;
        }
        // Keyboard use without a mouse: Space / Enter start, Delete / Backspace clear.
        if (message == WM_KEYDOWN && (wparam == VK_SPACE || wparam == VK_RETURN)) {
            start_capture(window, state);
            return 0;
        }
        if (message == WM_KEYDOWN && (wparam == VK_DELETE || wparam == VK_BACK)) {
            accept(window, state, KeyChoice::none());
            return 0;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (state && state->capturing) {
            on_key_up(window, state, static_cast<UINT>(wparam));
            return 0;  // no menu activation on Alt / F10
        }
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (state && state->capturing) return 0;  // no beep
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            paint(window, state);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void register_class() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = key_capture_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    registered = RegisterClassW(&wc) != 0;
}

}  // namespace

HWND create_key_capture(int id, int x, int y, int width, int height, HWND parent, bool allow_tap) {
    register_class();
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, width,
                           height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr),
                           allow_tap ? reinterpret_cast<LPVOID>(1) : nullptr);
}

void key_capture_set(HWND control, const KeyChoice& choice) {
    if (State* state = state_of(control)) {
        state->choice = choice;
        state->capturing = false;
        state->message = nullptr;
        InvalidateRect(control, nullptr, TRUE);
    }
}

KeyChoice key_capture_get(HWND control) {
    const State* state = state_of(control);
    return state ? state->choice : KeyChoice::none();
}

std::wstring key_choice_text(const KeyChoice& choice, const wchar_t* none) {
    switch (choice.kind) {
    case KeyChoice::Kind::kTap:
        return choice.tap_key == VK_CONTROL ? L"Ctrl" : L"Shift";
    case KeyChoice::Kind::kCombo: {
        const std::string text = keyboard_shortcut_string(choice.combo);
        std::wstring out;
        for (size_t i = 0; i < text.size(); ++i) {
            // "Ctrl+Shift+E" -> "Ctrl + Shift + E" (a trailing "+" is the key itself)
            if (text[i] == '+' && i + 1 < text.size()) {
                out += L" + ";
            } else {
                out += static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
            }
        }
        return out;
    }
    case KeyChoice::Kind::kNone:
    default:
        return none;
    }
}

bool is_reserved_shortcut(const KeyboardShortcut& shortcut) {
    const KeyboardShortcut punctuation = {kKeyModifierControl, VK_OEM_PERIOD};
    const KeyboardShortcut shape = {kKeyModifierShift, VK_SPACE};
    return shortcut == punctuation || shortcut == shape;
}

}  // namespace settings
}  // namespace cxxime
