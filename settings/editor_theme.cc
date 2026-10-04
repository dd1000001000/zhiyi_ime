// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Light and dark settings window, following the IME theme (General > Theme): window colors,
// the dark title bar, Windows' dark control styles (DarkMode_Explorer / DarkMode_CFD), and
// check box and radio button text, which the dark style leaves black.

#include "editor_app.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <algorithm>
#include <cwchar>

#include "editor_app_internal.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace cxxime {
namespace settings {
namespace {

constexpr DWORD kUseImmersiveDarkMode = 20;          // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kUseImmersiveDarkModeBefore20H1 = 19;  // Windows 10 1809 - 1909

UiColors g_colors = {};
HBRUSH g_window_brush = nullptr;
HBRUSH g_control_brush = nullptr;

bool has_class(HWND window, const wchar_t* name) {
    wchar_t class_name[32] = {};
    GetClassNameW(window, class_name, 32);
    return _wcsicmp(class_name, name) == 0;
}

// Check boxes and radio buttons (not push buttons).
bool is_choice_button(HWND window) {
    if (!has_class(window, L"Button")) return false;
    const LONG type = GetWindowLongW(window, GWL_STYLE) & BS_TYPEMASK;
    return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON ||
           type == BS_AUTORADIOBUTTON || type == BS_3STATE || type == BS_AUTO3STATE;
}

void style_control(HWND window, bool dark) {
    const wchar_t* style = nullptr;  // nullptr: the normal (light) style
    if (has_class(window, L"ComboBox")) {
        style = dark ? L"DarkMode_CFD" : nullptr;
    } else if (has_class(window, L"Button") || has_class(window, L"Edit") ||
               has_class(window, L"ListBox")) {
        style = dark ? L"DarkMode_Explorer" : nullptr;
    } else if (has_class(window, WC_LINK)) {
        // Links take the color of WM_CTLCOLORSTATIC (a lighter blue when dark).
        LITEM item = {};
        item.mask = LIF_ITEMINDEX | LIF_STATE;
        item.stateMask = LIS_DEFAULTCOLORS;
        item.state = dark ? LIS_DEFAULTCOLORS : 0;
        SendMessageW(window, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
        return;
    } else {
        return;
    }
    SetWindowTheme(window, style, nullptr);
}

BOOL CALLBACK style_child(HWND window, LPARAM dark) {
    style_control(window, dark != 0);
    return TRUE;
}

}  // namespace

const UiColors& ui_colors() {
    if (!g_window_brush) set_ui_dark(false);
    return g_colors;
}

void set_ui_dark(bool dark) {
    if (dark) {
        g_colors = {true,
                    RGB(32, 32, 32),     // window
                    RGB(240, 240, 240),  // text
                    RGB(160, 160, 160),  // hint
                    RGB(45, 45, 45),     // control
                    RGB(110, 170, 255),  // link
                    RGB(0, 95, 184)};    // selected page
    } else {
        g_colors = {false,
                    GetSysColor(COLOR_WINDOW),
                    GetSysColor(COLOR_WINDOWTEXT),
                    RGB(110, 110, 110),
                    GetSysColor(COLOR_WINDOW),
                    RGB(0, 102, 204),
                    RGB(0, 122, 215)};
    }
    for (HBRUSH* brush : {&g_window_brush, &g_control_brush}) {
        if (*brush) DeleteObject(*brush);
    }
    g_window_brush = CreateSolidBrush(g_colors.window);
    g_control_brush = CreateSolidBrush(g_colors.control);
}

HBRUSH window_brush() {
    ui_colors();
    return g_window_brush;
}

HBRUSH control_brush() {
    ui_colors();
    return g_control_brush;
}

void EditorApp::apply_ui_theme(bool dark, bool force) {
    if (!force && dark == ui_colors().dark) return;
    set_ui_dark(dark);
    const BOOL use_dark = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, kUseImmersiveDarkMode, &use_dark, sizeof(use_dark)))) {
        DwmSetWindowAttribute(hwnd_, kUseImmersiveDarkModeBefore20H1, &use_dark, sizeof(use_dark));
    }
    EnumChildWindows(hwnd_, style_child, dark ? 1 : 0);
    // Windows 10 repaints the title bar only after a frame change.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

LRESULT EditorApp::control_colors(UINT message, HDC dc, HWND control) {
    const UiColors& colors = ui_colors();
    SetBkColor(dc, colors.window);
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetTextColor(dc, colors.text);
        if (control == hList_) return reinterpret_cast<LRESULT>(window_brush());
        SetBkColor(dc, colors.control);
        return reinterpret_cast<LRESULT>(control_brush());
    }
    if (control == hFooter_) {
        SetBkMode(dc, TRANSPARENT);  // filled, so the old text goes when the language changes
    }
    if (has_class(control, L"Edit")) {  // read-only edit boxes (release notes)
        SetTextColor(dc, colors.text);
        SetBkColor(dc, colors.control);
        return reinterpret_cast<LRESULT>(control_brush());
    }
    if (std::find(hints_.begin(), hints_.end(), control) != hints_.end()) {
        SetTextColor(dc, colors.hint);
    } else if (has_class(control, WC_LINK)) {
        SetTextColor(dc, colors.link);
    } else {
        SetTextColor(dc, colors.text);
    }
    return reinterpret_cast<LRESULT>(window_brush());
}

// Dark check boxes and radio buttons: the dark style's box or circle, and the text in the
// window's text color. Light ones are drawn by Windows.
bool EditorApp::draw_choice_button(LPARAM notification, LRESULT* result) {
    auto* draw = reinterpret_cast<LPNMCUSTOMDRAW>(notification);
    if (!draw || draw->hdr.code != NM_CUSTOMDRAW || !ui_colors().dark ||
        !is_choice_button(draw->hdr.hwndFrom)) {
        return false;
    }
    if (draw->dwDrawStage != CDDS_PREPAINT) {
        *result = CDRF_DODEFAULT;
        return true;
    }
    const HWND button = draw->hdr.hwndFrom;
    const HDC dc = draw->hdc;
    const RECT rect = draw->rc;
    FillRect(dc, &rect, window_brush());

    const LONG type = GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool disabled = (draw->uItemState & CDIS_DISABLED) != 0;
    const bool pressed = (draw->uItemState & CDIS_SELECTED) != 0;
    const bool hot = (draw->uItemState & CDIS_HOT) != 0;
    // CBS_* and RBS_* share the order: unchecked normal/hot/pressed/disabled, then checked.
    int state = disabled ? CBS_UNCHECKEDDISABLED
                : pressed ? CBS_UNCHECKEDPRESSED
                : hot     ? CBS_UNCHECKEDHOT
                          : CBS_UNCHECKEDNORMAL;
    if (checked) state += CBS_CHECKEDNORMAL - CBS_UNCHECKEDNORMAL;
    const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;

    SIZE box = {S(13), S(13)};
    HTHEME theme = OpenThemeData(button, L"Button");
    if (theme) GetThemePartSize(theme, dc, part, state, nullptr, TS_DRAW, &box);
    const int top = rect.top + (rect.bottom - rect.top - box.cy) / 2;
    RECT box_rect = {rect.left, top, rect.left + box.cx, top + box.cy};
    if (theme) {
        DrawThemeBackground(theme, dc, part, state, &box_rect, nullptr);
        CloseThemeData(theme);
    } else {
        DrawFrameControl(dc, &box_rect, DFC_BUTTON,
                         (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) |
                             (checked ? DFCS_CHECKED : 0));
    }

    wchar_t text[256] = {};
    GetWindowTextW(button, text, 256);
    HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT,
                                                                             0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? ui_colors().hint : ui_colors().text);
    RECT text_rect = {box_rect.right + S(5), rect.top, rect.right, rect.bottom};
    DrawTextW(dc, text, -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    if ((draw->uItemState & CDIS_FOCUS) != 0 &&
        (SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
        RECT focus = text_rect;
        DrawTextW(dc, text, -1, &focus, DT_SINGLELINE | DT_LEFT | DT_CALCRECT);
        const int height = focus.bottom - focus.top;
        focus.top = rect.top + (rect.bottom - rect.top - height) / 2;
        focus.bottom = focus.top + height;
        InflateRect(&focus, 1, 1);
        DrawFocusRect(dc, &focus);
    }
    SelectObject(dc, old_font);
    *result = CDRF_SKIPDEFAULT;
    return true;
}

}  // namespace settings
}  // namespace cxxime
