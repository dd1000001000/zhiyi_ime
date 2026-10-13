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

constexpr wchar_t kClassName[] = L"ZhiyiKeyCapture";
constexpr UINT_PTR kNoticeTimer = 1;
constexpr UINT kNoticeMs = 2500;
// A key the capture hook took: wParam the key, lParam the modifiers held.
constexpr UINT kCapturedKey = WM_APP + 0x4B;

struct State {
    KeyChoice choice;
    bool allow_tap = false;
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
    // A tapped modifier may stay generic (either side) or keep its side (see key_capture.h).
    bool distinguish_side = false;
    uint32_t captured_modifier_vk = 0;  // the last modifier pressed, with its side
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

// A modifier key as the window message reports it, with its side from the scan code (right Shift,
// 0x36) or the extended flag (right Ctrl / right Alt). Any other key passes unchanged.
uint32_t modifier_vk_with_side(UINT vk, LPARAM lparam) {
    const uint32_t flags = static_cast<uint32_t>(lparam);
    const bool extended = (flags & (1u << 24)) != 0;
    const uint32_t scan = (flags >> 16) & 0xFF;
    switch (vk) {
    case VK_SHIFT:
        return scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL:
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

// The modifier family of a tap key (the generic VK) and its side (0 either, 1 left, 2 right).
uint32_t modifier_family(uint32_t vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return VK_SHIFT;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return VK_CONTROL;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return VK_MENU;
    default:
        return 0;
    }
}

uint32_t modifier_side(uint32_t vk) {
    switch (vk) {
    case VK_LSHIFT:
    case VK_LCONTROL:
    case VK_LMENU:
        return 1;
    case VK_RSHIFT:
    case VK_RCONTROL:
    case VK_RMENU:
        return 2;
    default:
        return 0;
    }
}

// Two tapped modifiers overlap when they share a family and either is side-agnostic or both are
// the same side. Left Ctrl and right Ctrl do not overlap; Ctrl and left Ctrl do.
bool tap_keys_overlap(uint32_t left, uint32_t right) {
    const uint32_t family = modifier_family(left);
    if (family == 0 || family != modifier_family(right)) return family == modifier_family(right);
    const uint32_t left_side = modifier_side(left);
    const uint32_t right_side = modifier_side(right);
    return left_side == 0 || right_side == 0 || left_side == right_side;
}

// "Ctrl", "Right Ctrl" / "右 Ctrl" for a tapped modifier key.
std::wstring tapped_modifier_text(uint32_t vk) {
    const wchar_t* mod = nullptr;
    const wchar_t* side = nullptr;
    switch (vk) {
    case VK_CONTROL: mod = tr("keys.mod_ctrl"); break;
    case VK_LCONTROL: mod = tr("keys.mod_ctrl"); side = tr("keys.side_left"); break;
    case VK_RCONTROL: mod = tr("keys.mod_ctrl"); side = tr("keys.side_right"); break;
    case VK_SHIFT: mod = tr("keys.mod_shift"); break;
    case VK_LSHIFT: mod = tr("keys.mod_shift"); side = tr("keys.side_left"); break;
    case VK_RSHIFT: mod = tr("keys.mod_shift"); side = tr("keys.side_right"); break;
    case VK_MENU: mod = tr("keys.mod_alt"); break;
    case VK_LMENU: mod = tr("keys.mod_alt"); side = tr("keys.side_left"); break;
    case VK_RMENU: mod = tr("keys.mod_alt"); side = tr("keys.side_right"); break;
    default: return {};
    }
    std::wstring text;
    if (side) {
        text += side;
        text += L' ';
    }
    text += mod;
    return text;
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
        state->captured_modifier_vk = modifier_vk_with_side(vk, lparam);
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
    if (is_common_app_shortcut(shortcut)) {
        reject(window, state, tr("keys.common"));
        return;
    }
    if (!is_valid_input_mode_shortcut(shortcut)) {
        reject(window, state, is_valid_keyboard_shortcut(shortcut) ? tr("keys.need_combo")
                                                                   : tr("keys.unsupported"));
        return;
    }
    accept(window, state, KeyChoice::of(shortcut));
}

void on_key_up(HWND window, State* state, UINT vk) {
    const uint32_t bit = modifier_bit(vk);
    if (bit == 0) return;
    // A modifier pressed and released alone: a tap. Shift, Ctrl and Alt all qualify, each with its
    // side when this box tells the two apart.
    if (!state->other_key_seen && state->modifiers_seen == bit) {
        if (state->allow_tap) {
            uint32_t tap_vk =
                state->distinguish_side ? state->captured_modifier_vk : generic_modifier_vk(bit);
            if (tap_vk == 0) tap_vk = generic_modifier_vk(bit);
            accept(window, state, KeyChoice::tap(tap_vk));
            return;
        }
        reject(window, state, tr("keys.need_combo"));
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
    RECT text_rect = {rect.left + S(8), rect.top, rect.right - S(6), rect.bottom};
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
        RECT note_rect = {measured.right + S(10), rect.top, rect.right - S(6), rect.bottom};
        DrawTextW(dc, state->note.c_str(), -1, &note_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, get_font());
        DeleteObject(small);
    }
    SelectObject(dc, old_font);
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
        if (choice.kind == KeyChoice::Kind::kTap) {
            state->distinguish_side = modifier_side(choice.tap_key) != 0;
            state->captured_modifier_vk = choice.tap_key;
        }
        InvalidateRect(control, nullptr, TRUE);
    }
}

void key_capture_set_distinguish(HWND control, bool distinguish) {
    State* state = state_of(control);
    if (!state) return;
    state->distinguish_side = distinguish;
    if (state->choice.kind == KeyChoice::Kind::kTap) {
        const uint32_t source =
            state->captured_modifier_vk ? state->captured_modifier_vk : state->choice.tap_key;
        const uint32_t tap_vk =
            distinguish ? source : generic_modifier_vk(modifier_bit(source));
        if (tap_vk != 0) state->choice = KeyChoice::tap(tap_vk);
    }
    InvalidateRect(control, nullptr, TRUE);
}

bool key_capture_get_distinguish(HWND control) {
    const State* state = state_of(control);
    return state && state->distinguish_side;
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
