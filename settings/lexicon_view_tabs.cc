// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "lexicon_view_tabs.h"

#include <commctrl.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr wchar_t kHotProperty[] = L"ZhiyiIME.LexiconViewTabHot";
constexpr wchar_t kNextProperty[] = L"ZhiyiIME.LexiconViewTabNext";
constexpr wchar_t kPreviousProperty[] = L"ZhiyiIME.LexiconViewTabPrevious";

void set_tab_stop(HWND tab, bool enabled) {
    LONG_PTR style = GetWindowLongPtrW(tab, GWL_STYLE);
    style = enabled ? style | WS_TABSTOP : style & ~static_cast<LONG_PTR>(WS_TABSTOP);
    SetWindowLongPtrW(tab, GWL_STYLE, style);
}

LRESULT CALLBACK view_tab_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                               UINT_PTR subclass_id, DWORD_PTR reference_data) {
    switch (message) {
    case WM_GETDLGCODE:
        return DefSubclassProc(window, message, wparam, lparam) | DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wparam == VK_LEFT || wparam == VK_RIGHT) {
            const wchar_t* property = wparam == VK_RIGHT ? kNextProperty : kPreviousProperty;
            const HWND peer = reinterpret_cast<HWND>(GetPropW(window, property));
            if (peer) {
                SendMessageW(peer, BM_CLICK, 0, 0);
                SetFocus(peer);
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (!GetPropW(window, kHotProperty)) {
            SetPropW(window, kHotProperty, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(window, kHotProperty);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        RemovePropW(window, kHotProperty);
        RemovePropW(window, kNextProperty);
        RemovePropW(window, kPreviousProperty);
        RemoveWindowSubclass(window, view_tab_proc, subclass_id);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

HWND create_view_tab(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height,
                     HFONT font, bool tab_stop) {
    const DWORD tab_style = tab_stop ? WS_TABSTOP : 0;
    HWND tab = CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_FLAT | tab_style, x, y, width,
        height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr),
        nullptr);
    SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return tab;
}

} // namespace

LexiconViewTabs create_lexicon_view_tabs(HWND parent, int entries_id, int candidate_order_id,
                                         int preferences_id, int x, int y, int width, int height,
                                         HFONT font) {
    const int entries_width = width / 3;
    const int order_width = width / 3;
    LexiconViewTabs tabs;
    tabs.entries =
        create_view_tab(parent, entries_id, L"词条", x, y, entries_width, height, font, true);
    tabs.candidate_order = create_view_tab(parent, candidate_order_id, L"候选排序",
                                           x + entries_width, y, order_width, height, font, false);
    tabs.preferences = create_view_tab(parent, preferences_id, L"选词偏好",
                                       x + entries_width + order_width, y,
                                       width - entries_width - order_width, height, font, false);
    SetPropW(tabs.entries, kPreviousProperty, tabs.preferences);
    SetPropW(tabs.entries, kNextProperty, tabs.candidate_order);
    SetPropW(tabs.candidate_order, kPreviousProperty, tabs.entries);
    SetPropW(tabs.candidate_order, kNextProperty, tabs.preferences);
    SetPropW(tabs.preferences, kPreviousProperty, tabs.candidate_order);
    SetPropW(tabs.preferences, kNextProperty, tabs.entries);
    SetWindowSubclass(tabs.entries, view_tab_proc, 1, 0);
    SetWindowSubclass(tabs.candidate_order, view_tab_proc, 1, 0);
    SetWindowSubclass(tabs.preferences, view_tab_proc, 1, 0);
    return tabs;
}

void select_lexicon_view_tab(const LexiconViewTabs& tabs, HWND selected) {
    set_tab_stop(tabs.entries, selected == tabs.entries);
    set_tab_stop(tabs.candidate_order, selected == tabs.candidate_order);
    set_tab_stop(tabs.preferences, selected == tabs.preferences);
    InvalidateRect(tabs.entries, nullptr, FALSE);
    InvalidateRect(tabs.candidate_order, nullptr, FALSE);
    InvalidateRect(tabs.preferences, nullptr, FALSE);
}

bool draw_lexicon_view_tab(const DRAWITEMSTRUCT& item, const LexiconViewTabs& tabs, HWND selected) {
    if (item.hwndItem != tabs.entries && item.hwndItem != tabs.candidate_order &&
        item.hwndItem != tabs.preferences) {
        return false;
    }

    const bool is_selected = item.hwndItem == selected;
    const bool hot = GetPropW(item.hwndItem, kHotProperty) != nullptr;
    FillRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_BTNFACE));

    wchar_t text[32] = {};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(item.hDC, font) : nullptr;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC,
                 is_selected || hot ? GetSysColor(COLOR_HIGHLIGHT)
                                    : GetSysColor(COLOR_BTNTEXT));
    RECT text_rect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    if (is_selected) {
        const int underline_height = S(2) > 1 ? S(2) : 2;
        RECT underline = {item.rcItem.left + S(8), item.rcItem.bottom - underline_height,
                          item.rcItem.right - S(8), item.rcItem.bottom};
        FillRect(item.hDC, &underline, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    if (old_font) {
        SelectObject(item.hDC, old_font);
    }
    return true;
}

} // namespace settings
} // namespace cxxime
