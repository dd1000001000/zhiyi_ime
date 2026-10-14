// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "key_capture.h"

#include <algorithm>
#include <cwchar>
#include <optional>

#include <imm.h>

#include <cxxime/key_event.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

constexpr wchar_t kClassName[] = L"ZhiyuKeyCapture";
constexpr UINT_PTR kNoticeTimer = 1;
constexpr UINT kNoticeMs = 2500;
constexpr UINT_PTR kDoubleTimer = 2;
constexpr UINT kDoubleMs = 350;  // a second press within this doubles the key
// A key the capture hook took: wParam the key, lParam the modifiers held.
constexpr UINT kCapturedKey = WM_APP + 0x4B;

struct State {
    KeyChoice choice;
    bool allow_tap = false;
    bool allow_double = false;      // a second press of the same key doubles it
    bool allow_common = false;      // a common application shortcut (Ctrl+C) is allowed
    bool waiting_double = false;    // the first press is in, waiting for a second one
    KeyChoice pending_double;       // that first press
    bool capturing = false;
    bool hover = false;
    const wchar_t* message = nullptr;  // why the last key was not taken (while capturing)
    std::wstring notice;               // why the key was kept (shown for a moment afterwards)
    KeyNote note_kind = KeyNote::kNone;
    std::wstring note;                 // shown after the key
    KeyCaptureCheck check;
    // The key presses of one capture: modifiers seen, and whether another key came with them.
    uint32_t modifiers_seen = 0;
    bool other_key_seen = false;
    bool arrow_hover = false;  // the mouse is over the side arrow
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

// The generic modifier of a bit, either side (VK_SHIFT / VK_CONTROL / VK_MENU).
uint32_t generic_modifier_vk(uint32_t bit) {
    switch (bit) {
    case kKeyModifierShift:
        return VK_SHIFT;
    case kKeyModifierControl:
        return VK_CONTROL;
    case kKeyModifierAlt:
        return VK_MENU;
    default:
        return 0;
    }
}

// "Ctrl", "Right Ctrl" / "右 Ctrl" for a tapped modifier key.
std::wstring tapped_modifier_text(uint32_t vk) {
    const wchar_t* mod = nullptr;
    switch (tap_family(vk)) {
    case VK_SHIFT: mod = tr("keys.mod_shift"); break;
    case VK_CONTROL: mod = tr("keys.mod_ctrl"); break;
    default: return {};
    }
    std::wstring text;
    switch (tap_side(vk)) {
    case TapSide::kLeft: text = std::wstring(tr("keys.side_left")) + L' '; break;
    case TapSide::kRight: text = std::wstring(tr("keys.side_right")) + L' '; break;
    case TapSide::kEither: break;
    }
    return text + mod;
}

// The arrow at the right end of a box holding a tapped modifier: picks the side.
bool shows_arrow(const State* state) {
    return state->allow_tap && !state->capturing && state->notice.empty() &&
           state->choice.kind == KeyChoice::Kind::kTap;
}

RECT arrow_rect(HWND window) {
    RECT rect;
    GetClientRect(window, &rect);
    return {rect.right - S(28), rect.top + 1, rect.right - 1, rect.bottom - 1};
}

bool over_arrow(HWND window, const State* state, LPARAM lparam) {
    const POINT point = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    const RECT arrow = arrow_rect(window);
    return shows_arrow(state) && PtInRect(&arrow, point);
}

uint32_t modifiers_down() {
    uint32_t modifiers = 0;
    if (key_down(VK_SHIFT)) modifiers |= kKeyModifierShift;
    if (key_down(VK_CONTROL)) modifiers |= kKeyModifierControl;
    if (key_down(VK_MENU)) modifiers |= kKeyModifierAlt;
    return modifiers;
}

// While a box captures, a low-level keyboard hook takes the keys before Windows hands them to
// global hotkeys (other programs', this IME's activation key) or to input methods, which
// would act on them while the box never saw them. Modifiers pass (the box reads their state,
// a tapped one is a choice); so do the keys that leave the window (Win, Alt+Tab, Alt+Esc,
// Ctrl+Esc): the capture ends when the box loses the focus.
HHOOK g_capture_hook = nullptr;
HWND g_capture_box = nullptr;
DWORD g_capture_key_down = 0;  // a taken key, until its key-up

bool async_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

LRESULT CALLBACK capture_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION || !g_capture_box) return CallNextHookEx(nullptr, code, wparam, lparam);
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    const DWORD vk = key->vkCode;
    if (!down && vk == g_capture_key_down) {
        g_capture_key_down = 0;
        return 1;
    }
    const bool alt = async_down(VK_MENU);
    const bool leaves_window = vk == VK_LWIN || vk == VK_RWIN || async_down(VK_LWIN) ||
                               async_down(VK_RWIN) || (alt && (vk == VK_TAB || vk == VK_ESCAPE)) ||
                               (async_down(VK_CONTROL) && vk == VK_ESCAPE && !alt);
    if (!down || modifier_bit(vk) != 0 || leaves_window || GetFocus() != g_capture_box) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    if (vk != g_capture_key_down) {  // not an auto-repeat
        uint32_t modifiers = 0;
        if (async_down(VK_SHIFT)) modifiers |= kKeyModifierShift;
        if (async_down(VK_CONTROL)) modifiers |= kKeyModifierControl;
        if (alt) modifiers |= kKeyModifierAlt;
        g_capture_key_down = vk;
        PostMessageW(g_capture_box, kCapturedKey, vk, modifiers);
    }
    return 1;
}

void remove_capture_hook(HWND window) {
    if (g_capture_box != window) return;
    if (g_capture_hook) UnhookWindowsHookEx(g_capture_hook);
    g_capture_hook = nullptr;
    g_capture_box = nullptr;
    g_capture_key_down = 0;
}

void install_capture_hook(HWND window) {
    if (g_capture_box && g_capture_box != window) remove_capture_hook(g_capture_box);
    g_capture_box = window;
    if (!g_capture_hook) {
        // Without it (it may fail) the box still gets the keys nothing else takes.
        g_capture_hook = SetWindowsHookExW(WH_KEYBOARD_LL, capture_hook_proc,
                                           GetModuleHandleW(nullptr), 0);
    }
}

void notify_changed(HWND window) {
    SendMessageW(GetParent(window), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window), kKeyCaptureChanged),
                 reinterpret_cast<LPARAM>(window));
}

void clear_notice(HWND window, State* state) {
    if (state->notice.empty()) return;
    state->notice.clear();
    KillTimer(window, kNoticeTimer);
    InvalidateRect(window, nullptr, TRUE);
}

void start_capture(HWND window, State* state) {
    clear_notice(window, state);
    state->capturing = true;
    state->message = nullptr;
    state->modifiers_seen = 0;
    state->other_key_seen = false;
    install_capture_hook(window);
    InvalidateRect(window, nullptr, TRUE);
}

void stop_capture(HWND window, State* state) {
    remove_capture_hook(window);
    state->capturing = false;
    state->message = nullptr;
    InvalidateRect(window, nullptr, TRUE);
}

void accept(HWND window, State* state, const KeyChoice& choice) {
    if (choice.kind != KeyChoice::Kind::kNone && state->check) {
        std::wstring notice = state->check(choice);
        if (!notice.empty()) {
            // Used by another box: keep the previous key.
            stop_capture(window, state);
            state->notice = std::move(notice);
            SetTimer(window, kNoticeTimer, kNoticeMs, nullptr);
            return;
        }
    }
    // A box that takes a double press: the second press of the same key within a moment stores it
    // as "Ctrl+C+C" (repeat 2).
    if (state->allow_double && choice.kind == KeyChoice::Kind::kCombo) {
        if (state->waiting_double && state->pending_double.kind == KeyChoice::Kind::kCombo &&
            state->pending_double.combo.modifiers == choice.combo.modifiers &&
            state->pending_double.combo.virtual_key == choice.combo.virtual_key) {
            KeyboardShortcut doubled = choice.combo;
            doubled.repeat = 2;
            state->choice = KeyChoice::of(doubled);
            state->waiting_double = false;
            KillTimer(window, kDoubleTimer);
            clear_notice(window, state);
            stop_capture(window, state);
            notify_changed(window);
            return;
        }
        // The first press: remember it (still capturing) and wait for a second one.
        state->choice = choice;
        state->pending_double = choice;
        state->waiting_double = true;
        SetTimer(window, kDoubleTimer, kDoubleMs, nullptr);
        clear_notice(window, state);
        InvalidateRect(window, nullptr, TRUE);
        return;
    }
    clear_notice(window, state);
    state->choice = choice;
    stop_capture(window, state);
    notify_changed(window);
}

void reject(HWND window, State* state, const wchar_t* message) {
    state->message = message;
    InvalidateRect(window, nullptr, TRUE);
}

// `modifiers`: held with the key (a key from the capture hook), else read from the key state.
void on_key_down(HWND window, State* state, UINT vk, LPARAM lparam,
                 std::optional<uint32_t> modifiers = std::nullopt) {
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
    if (vk == VK_PROCESSKEY) {
        // An input method took the key (e.g. its own Ctrl+Space): the scan code tells which.
        const UINT scan = static_cast<UINT>((lparam >> 16) & 0xFF) |
                          ((lparam & (1 << 24)) != 0 ? 0xE000 : 0);
        vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    }
    state->other_key_seen = true;
    const KeyboardShortcut shortcut = {modifiers ? *modifiers : modifiers_down(), vk};
    if (vk == VK_ESCAPE && shortcut.modifiers == 0) {
        stop_capture(window, state);  // cancel, keep the key
        return;
    }
    if (!state->allow_common && is_common_app_shortcut(shortcut)) {
        reject(window, state, tr("keys.common"));
        return;
    }
    const bool valid = state->allow_common ? is_valid_translate_shortcut(shortcut)
                                           : is_valid_input_mode_shortcut(shortcut);
    if (!valid) {
        reject(window, state, is_valid_keyboard_shortcut(shortcut) ? tr("keys.need_combo")
                                                                   : tr("keys.unsupported"));
        return;
    }
    accept(window, state, KeyChoice::of(shortcut));
}

void on_key_up(HWND window, State* state, UINT vk) {
    const uint32_t bit = modifier_bit(vk);
    if (bit == 0) return;
    // A modifier pressed and released alone: a tap of Shift or Ctrl. It is taken as either side,
    // whichever was pressed; the arrow picks one side (show_side_menu). Not Alt: Windows does not
    // hand Alt keys to input methods, so a tapped Alt would never switch anything.
    if (!state->other_key_seen && state->modifiers_seen == bit) {
        if (state->allow_tap && (bit == kKeyModifierShift || bit == kKeyModifierControl)) {
            accept(window, state, KeyChoice::tap(generic_modifier_vk(bit)));
            return;
        }
        reject(window, state, state->allow_tap ? tr("keys.tap_only") : tr("keys.need_combo"));
    }
    if (modifiers_down() == 0) {
        state->modifiers_seen = 0;
        state->other_key_seen = false;
    }
}

// The arrow's menu: either side, only the left one, only the right one.
void show_side_menu(HWND window, State* state) {
    const uint32_t family = tap_family(state->choice.tap_key);
    if (family == 0) return;
    auto only = [](uint32_t vk) {
        std::wstring text = tr("keys.side_only");
        const size_t at = text.find(L"{0}");
        if (at != std::wstring::npos) text.replace(at, 3, tapped_modifier_text(vk));
        return text;
    };
    const std::wstring left = only(tap_key_on_side(family, TapSide::kLeft));
    const std::wstring right = only(tap_key_on_side(family, TapSide::kRight));
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, tr("keys.side_either"));
    AppendMenuW(menu, MF_STRING, 2, left.c_str());
    AppendMenuW(menu, MF_STRING, 3, right.c_str());
    // Menu ids follow TapSide: 1 either, 2 left, 3 right.
    CheckMenuRadioItem(menu, 1, 3, 1 + static_cast<UINT>(tap_side(state->choice.tap_key)),
                       MF_BYCOMMAND);
    RECT box;
    GetWindowRect(window, &box);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN |
                                                  TPM_NONOTIFY | TPM_LEFTBUTTON,
                                        box.right, box.bottom, 0, window, nullptr);
    DestroyMenu(menu);
    if (command < 1 || command > 3) return;
    const uint32_t vk = tap_key_on_side(family, static_cast<TapSide>(command - 1));
    if (vk != state->choice.tap_key) accept(window, state, KeyChoice::tap(vk));
}

void paint(HWND window, State* state) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);
    RECT rect;
    GetClientRect(window, &rect);
    const UiColors& colors = ui_colors();
    const bool dark = colors.dark;
    FillRect(dc, &rect, control_brush());

    const bool focused = GetFocus() == window;
    const bool show_note = !state->capturing && state->notice.empty() && !state->note.empty() &&
                           state->choice.kind != KeyChoice::Kind::kNone;
    const bool warning = show_note && state->note_kind == KeyNote::kWarning;
    if (warning) {
        HBRUSH yellow = CreateSolidBrush(dark ? RGB(70, 56, 18) : RGB(255, 247, 214));
        FillRect(dc, &rect, yellow);
        DeleteObject(yellow);
    }
    const COLORREF blue = dark ? RGB(96, 170, 255) : RGB(0, 103, 192);
    const COLORREF red = dark ? RGB(255, 120, 108) : RGB(196, 43, 28);
    const COLORREF border = state->capturing ? blue
                            : warning         ? RGB(222, 170, 40)
                            : state->hover    ? (dark ? RGB(150, 150, 150) : RGB(120, 120, 120))
                                              : (dark ? RGB(95, 95, 95) : RGB(170, 170, 170));
    const int thickness = state->capturing ? (std::max)(2, S(2)) : 1;
    HBRUSH border_brush = CreateSolidBrush(border);
    for (int i = 0; i < thickness; ++i) {
        RECT frame = {rect.left + i, rect.top + i, rect.right - i, rect.bottom - i};
        FrameRect(dc, &frame, border_brush);
    }
    DeleteObject(border_brush);

    std::wstring text;
    COLORREF color = colors.text;
    if (state->capturing) {
        text = state->message ? state->message : tr("keys.press");
        color = state->message ? red : blue;
    } else if (!state->notice.empty()) {
        text = state->notice;
        color = red;
    } else {
        text = key_choice_text(state->choice, tr("keys.none"));
        if (state->choice.kind == KeyChoice::Kind::kNone) color = colors.hint;
    }
    HGDIOBJ old_font = SelectObject(dc, get_font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    const bool arrow = shows_arrow(state);
    const RECT arrow_area = arrow_rect(window);
    const LONG text_right = arrow ? arrow_area.left : rect.right - S(6);
    RECT text_rect = {rect.left + S(8), rect.top, text_right, rect.bottom};
    DrawTextW(dc, text.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (show_note) {
        // Smaller text after the key.
        RECT measured = text_rect;
        DrawTextW(dc, text.c_str(), -1, &measured,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_CALCRECT);
        LOGFONTW font = {};
        GetObjectW(get_font(), sizeof(font), &font);
        font.lfHeight = font.lfHeight * 4 / 5;
        HFONT small = CreateFontIndirectW(&font);
        SelectObject(dc, small);
        SetTextColor(dc, warning ? (dark ? RGB(240, 190, 90) : RGB(150, 98, 0)) : colors.hint);
        RECT note_rect = {measured.right + S(10), rect.top, text_right, rect.bottom};
        DrawTextW(dc, state->note.c_str(), -1, &note_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, get_font());
        DeleteObject(small);
    }
    SelectObject(dc, old_font);
    if (arrow) {
        if (state->arrow_hover) {
            HBRUSH hover = CreateSolidBrush(dark ? RGB(62, 62, 62) : RGB(229, 229, 229));
            FillRect(dc, &arrow_area, hover);
            DeleteObject(hover);
        }
        const int cx = (arrow_area.left + arrow_area.right) / 2;
        const int cy = (arrow_area.top + arrow_area.bottom) / 2;
        const int half = (std::max)(3, S(4));
        const POINT chevron[] = {{cx - half, cy - half / 2},
                                 {cx, cy + half / 2},
                                 {cx + half + 1, cy - half / 2 - 1}};
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(1)), colors.text);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        Polyline(dc, chevron, 3);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    if (focused && !state->capturing) {
        RECT focus = {rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3};
        SetTextColor(dc, colors.text);  // the dotted line takes the text color
        SetBkColor(dc, colors.control);
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
    case WM_TIMER:
        if (state && wparam == kNoticeTimer) {
            clear_notice(window, state);
            return 0;
        }
        if (state && wparam == kDoubleTimer) {
            KillTimer(window, kDoubleTimer);
            if (state->waiting_double) {
                state->waiting_double = false;
                stop_capture(window, state);  // no second press: the key stays single
                notify_changed(window);
            }
            return 0;
        }
        break;
    case kCapturedKey:
        if (state && state->capturing) {
            on_key_down(window, state, static_cast<UINT>(wparam), 0,
                        static_cast<uint32_t>(lparam));
        }
        return 0;
    case WM_NCDESTROY:
        remove_capture_hook(window);
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_GETDLGCODE:
        // While capturing every key (Tab, Enter, Esc, arrows) belongs to the box.
        if (state && state->capturing) return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTTAB;
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        if (state && over_arrow(window, state, lparam)) {
            show_side_menu(window, state);
        } else if (state) {
            start_capture(window, state);
        }
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
        if (state) {
            const bool arrow_hover = over_arrow(window, state, lparam);
            if (!state->hover || arrow_hover != state->arrow_hover) {
                if (!state->hover) {
                    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
                    TrackMouseEvent(&track);
                }
                state->hover = true;
                state->arrow_hover = arrow_hover;
                InvalidateRect(window, nullptr, TRUE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state) {
            state->hover = false;
            state->arrow_hover = false;
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
        // Keyboard use without a mouse: Space / Enter start, Delete / Backspace clear, Alt+Down
        // or F4 picks the side of a tapped modifier.
        if (shows_arrow(state) && ((message == WM_SYSKEYDOWN && wparam == VK_DOWN) ||
                                   (message == WM_KEYDOWN && wparam == VK_F4))) {
            show_side_menu(window, state);
            return 0;
        }
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
    HWND control = CreateWindowExW(
        0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
        allow_tap ? reinterpret_cast<LPVOID>(1) : nullptr);
    // No input method in the box: the active one (this IME included) would otherwise take its
    // own switch keys, such as Ctrl+Space, before the box sees them.
    if (control) ImmAssociateContextEx(control, nullptr, 0);
    return control;
}

void key_capture_set(HWND control, const KeyChoice& choice) {
    if (State* state = state_of(control)) {
        clear_notice(control, state);
        state->choice = choice;
        state->capturing = false;
        state->message = nullptr;
        InvalidateRect(control, nullptr, TRUE);
    }
}

void key_capture_set_allow_double(HWND control, bool allow_double) {
    if (State* state = state_of(control)) state->allow_double = allow_double;
}

void key_capture_set_allow_common(HWND control, bool allow_common) {
    if (State* state = state_of(control)) state->allow_common = allow_common;
}

void key_capture_set_note(HWND control, KeyNote kind, const std::wstring& text) {
    if (State* state = state_of(control)) {
        state->note_kind = text.empty() ? KeyNote::kNone : kind;
        state->note = text;
        InvalidateRect(control, nullptr, TRUE);
    }
}

void key_capture_set_check(HWND control, KeyCaptureCheck check) {
    if (State* state = state_of(control)) {
        state->check = std::move(check);
    }
}

bool same_key_choice(const KeyChoice& left, const KeyChoice& right) {
    if (left.kind != right.kind) return false;
    switch (left.kind) {
    case KeyChoice::Kind::kTap:
        return tap_keys_overlap(left.tap_key, right.tap_key);
    case KeyChoice::Kind::kCombo:
        return left.combo == right.combo;
    case KeyChoice::Kind::kNone:
    default:
        return true;
    }
}

KeyChoice key_capture_get(HWND control) {
    const State* state = state_of(control);
    return state ? state->choice : KeyChoice::none();
}

std::wstring key_choice_text(const KeyChoice& choice, const wchar_t* none) {
    switch (choice.kind) {
    case KeyChoice::Kind::kTap:
        return tapped_modifier_text(choice.tap_key);
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

}  // namespace settings
}  // namespace cxxime
