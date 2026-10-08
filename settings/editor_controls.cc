// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app_internal.h"

#include "i18n.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace cxxime {
namespace settings {
namespace {

struct Card {
    RECT rect;
    std::wstring title;
    bool visible = true;
};
struct OpenCard {
    int top = 0;
    std::wstring title;
};
std::map<HWND, std::vector<Card>> g_cards;
std::map<HWND, int> g_scroll;  // scrollable pages: how far they are scrolled
std::map<HWND, OpenCard> g_open_cards;

HFONT g_title_font = nullptr;
HFONT g_small_font = nullptr;
std::map<int, HFONT> g_icon_fonts;

int card_radius() { return S(8); }
int card_pad_top() { return S(12); }
int card_title_height() { return S(30); }
int card_pad_bottom() { return S(10); }
int card_gap() { return S(12); }

bool font_installed(const wchar_t* face) {
    LOGFONTW query = {};
    query.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(query.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &query,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found_flag) -> int {
            *reinterpret_cast<bool*>(found_flag) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

}  // namespace

float g_dpi = 1.0f;
HFONT g_hFont = nullptr;
int kListW = 0;
int kPadX = 0;
int kPadY = 0;
int kCtrlH = 0;
int kRowH = 0;
int kPanelPadTop = 0;
int kPanelPadLeft = 0;
int kLblW = 0;
int kCtlX = 0;

int S(int value) { return static_cast<int>(value * g_dpi + 0.5f); }

void init_layout() {
    kListW = S(176);
    kPadX = S(192);
    kPadY = S(16);
    kCtrlH = S(kFontPt + 14);
    kRowH = S(kFontPt + 20);
    kPanelPadTop = 0;
    kPanelPadLeft = S(20);  // inside a card
    kLblW = S(110);
    kCtlX = kPanelPadLeft + kLblW + S(8);
}

HFONT get_font() {
    if (!g_hFont) {
        g_hFont = CreateFontW(-S(kFontPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0,
                              L"Microsoft YaHei UI");
    }
    return g_hFont;
}

HFONT get_title_font() {
    if (!g_title_font) {
        g_title_font = CreateFontW(-S(kFontPt + 1), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    }
    return g_title_font;
}

HFONT get_small_font() {
    if (!g_small_font) {
        g_small_font = CreateFontW(-S(kFontPt - 2), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    }
    return g_small_font;
}

HFONT get_icon_font(int point) {
    HFONT& font = g_icon_fonts[point];
    if (!font) {
        static const wchar_t* const face =
            font_installed(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        font = CreateFontW(-S(point), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, face);
    }
    return font;
}

void release_shared_fonts() {
    for (HFONT* font : {&g_hFont, &g_title_font, &g_small_font}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    for (auto& [point, font] : g_icon_fonts) {
        if (font) DeleteObject(font);
    }
    g_icon_fonts.clear();
}

int card_begin(HWND panel, int top, const wchar_t* title) {
    g_open_cards[panel] = {top, title ? title : L""};
    return top + card_pad_top() + (title && *title ? card_title_height() : S(4));
}

int card_end(HWND panel, int content_bottom) {
    const OpenCard open = g_open_cards[panel];
    g_open_cards.erase(panel);
    RECT client = {};
    GetClientRect(panel, &client);
    const int bottom = content_bottom + card_pad_bottom();
    g_cards[panel].push_back({{0, open.top, client.right, bottom}, open.title});
    return bottom + card_gap();
}

void clear_cards() {
    g_cards.clear();
    g_open_cards.clear();
    g_scroll.clear();
}

void make_panel_scrollable(HWND panel) {
    // The bar shows from the start (disabled while everything fits), so the page is laid out
    // in the width that is left beside it.
    SetWindowLongPtrW(panel, GWL_STYLE, GetWindowLongPtrW(panel, GWL_STYLE) | WS_VSCROLL);
    SetWindowPos(panel, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SCROLLINFO info = {sizeof(info), SIF_ALL | SIF_DISABLENOSCROLL, 0, 0, 1, 0, 0};
    SetScrollInfo(panel, SB_VERT, &info, FALSE);
    g_scroll[panel] = 0;
}

void update_panel_scroll(HWND panel) {
    if (g_scroll.find(panel) == g_scroll.end()) return;
    int bottom = 0;
    for (const Card& card : g_cards[panel]) bottom = (std::max)(bottom, static_cast<int>(card.rect.bottom));
    RECT client = {};
    GetClientRect(panel, &client);
    SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL};
    info.nMin = 0;
    info.nMax = bottom + card_gap() + g_scroll[panel];
    info.nPage = static_cast<UINT>(client.bottom);
    SetScrollInfo(panel, SB_VERT, &info, TRUE);
}

namespace {

void scroll_panel_to(HWND panel, int position) {
    SCROLLINFO info = {sizeof(info), SIF_ALL};
    GetScrollInfo(panel, SB_VERT, &info);
    const int last = (std::max)(0, info.nMax - static_cast<int>(info.nPage) + 1);
    position = (std::max)(0, (std::min)(position, last));
    const int old = g_scroll[panel];
    if (position == old) return;
    g_scroll[panel] = position;
    info.fMask = SIF_POS;
    info.nPos = position;
    SetScrollInfo(panel, SB_VERT, &info, TRUE);
    // Cards are kept in page coordinates of the moment they were made: move them along.
    for (Card& card : g_cards[panel]) OffsetRect(&card.rect, 0, old - position);
    ScrollWindowEx(panel, 0, old - position, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    // The controls too, now: left to the idle loop they lag behind the page while the wheel
    // turns (about 2 ms for the General page).
    RedrawWindow(panel, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

// WM_VSCROLL / WM_MOUSEWHEEL on a scrollable page; false for any other page.
bool scroll_message(HWND panel, UINT message, WPARAM wparam) {
    if (g_scroll.find(panel) == g_scroll.end()) return false;
    SCROLLINFO info = {sizeof(info), SIF_ALL};
    GetScrollInfo(panel, SB_VERT, &info);
    int position = g_scroll[panel];
    const int line = kRowH;
    if (message == WM_MOUSEWHEEL) {
        position -= GET_WHEEL_DELTA_WPARAM(wparam) * line / WHEEL_DELTA * 2;
    } else {
        switch (LOWORD(wparam)) {
        case SB_LINEUP: position -= line; break;
        case SB_LINEDOWN: position += line; break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = info.nMax; break;
        default: return true;
        }
    }
    scroll_panel_to(panel, position);
    return true;
}

}  // namespace

void set_card_visible(HWND panel, int index, bool visible) {
    auto found = g_cards.find(panel);
    if (found == g_cards.end() || index < 0 || index >= static_cast<int>(found->second.size()) ||
        found->second[index].visible == visible) {
        return;
    }
    found->second[index].visible = visible;
    InvalidateRect(panel, &found->second[index].rect, TRUE);
}

void paint_panel(HWND panel, HDC dc) {
    RECT client = {};
    GetClientRect(panel, &client);
    FillRect(dc, &client, window_brush());
    const UiColors& colors = ui_colors();
    const auto found = g_cards.find(panel);
    if (found == g_cards.end()) return;
    HGDIOBJ old_font = SelectObject(dc, get_title_font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colors.text);
    for (const Card& card : found->second) {
        if (!card.visible) continue;
        RECT rect = card.rect;
        rect.right -= 1;  // the outline inside the page
        fill_round_rect(dc, rect, card_radius(), colors.card, colors.border);
        if (!card.title.empty()) {
            RECT title = {rect.left + kPanelPadLeft, rect.top + card_pad_top(),
                          rect.right - kPanelPadLeft, rect.top + card_pad_top() + card_title_height()};
            DrawTextW(dc, card.title.c_str(), -1, &title,
                      DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    SelectObject(dc, old_font);
}

int make_label(const wchar_t* text, int x, int y, HWND parent) {
    HDC dc = GetDC(parent);
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, get_font()));
    SIZE size;
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old_font);
    ReleaseDC(parent, dc);
    int width = size.cx + S(4);
    HWND control =
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT, x, y, width, kCtrlH,
                        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return x + width + S(8);
}

int label_width(std::initializer_list<const char*> keys) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old_font = SelectObject(dc, get_font());
    int width = 0;
    for (const char* key : keys) {
        const wchar_t* text = tr(key);
        SIZE size = {};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        width = (std::max)(width, static_cast<int>(size.cx));
    }
    SelectObject(dc, old_font);
    ReleaseDC(nullptr, dc);
    return width + S(8);
}

// Labels are centered on the row, like the text of the control next to them.
void make_aligned_label(const wchar_t* text, int y, HWND parent) {
    HWND control = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                                   kPanelPadLeft, y, kLblW, kCtrlH, parent, nullptr,
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
}

int make_aligned_label(const wchar_t* text, int x, int width, int y, HWND parent) {
    HWND control = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE, x, y, width,
                                   kCtrlH, parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return x + width + S(8);
}

HWND make_page_button(int id, const wchar_t* text, int x, int y, int width, int height,
                      HWND parent) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, width,
                                   height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    enable_button_hover(control);
    return control;
}

HWND make_button(int id, const wchar_t* text, int x, int y, int width, HWND parent) {
    return make_page_button(id, text, x, y, width, S(30), parent);
}

HWND make_edit(int id, int x, int y, int width, HWND parent) {
    HWND edit =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, x, y,
                        width, kCtrlH, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandle(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return edit;
}

namespace {

LRESULT CALLBACK ComboWheelProc(HWND combo, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                DWORD_PTR) {
    if (message == WM_MOUSEWHEEL && !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
        return SendMessageW(GetParent(combo), WM_MOUSEWHEEL, wparam, lparam);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(combo, ComboWheelProc, id);
    return DefSubclassProc(combo, message, wparam, lparam);
}

}  // namespace

void scroll_page_on_wheel(HWND combo) {
    if (combo) SetWindowSubclass(combo, ComboWheelProc, 1, 0);
}

HWND make_combo(int id, int x, int y, int width, HWND parent) {
    HWND combo =
        CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                        x, y, width, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandle(nullptr), nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    scroll_page_on_wheel(combo);
    return combo;
}

void set_combo_drop_count(HWND combo, int count) {
    if (!combo || count <= 0) {
        return;
    }
    RECT rect = {};
    GetWindowRect(combo, &rect);
    SetWindowPos(combo, nullptr, 0, 0, rect.right - rect.left, kCtrlH * (count + 1),
                 SWP_NOMOVE | SWP_NOZORDER);
}

HWND make_check(int id, const wchar_t* text, int x, int y, int width, HWND parent) {
    HWND check =
        CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x,
                                 y, width, kCtrlH, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandle(nullptr), nullptr);
    SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return check;
}

HWND make_radio(int id, const wchar_t* text, int x, int y, int width, HWND parent, bool group) {
    HWND radio = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | (group ? WS_GROUP : 0), x, y,
        width, kCtrlH, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(radio, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return radio;
}

void combo_add(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void combo_sel(HWND combo, const wchar_t* text) {
    int index = static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                 reinterpret_cast<LPARAM>(text)));
    if (index >= 0) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
}

std::wstring utf8_to_wstr(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        &result[0], length);
    return result;
}

std::string wstr_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        &result[0], length, nullptr, nullptr);
    return result;
}

std::string edit_text_utf8(HWND edit) {
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        return {};
    }
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(edit, &text[0], length + 1);
    text.resize(length);
    return wstr_to_utf8(text);
}

std::wstring path_for_display(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    return utf8_to_wstr(normalized);
}

void set_edit_int(HWND edit, int value) {
    wchar_t buffer[32];
    _itow_s(value, buffer, 10);
    SetWindowTextW(edit, buffer);
}

int get_edit_int(HWND edit) {
    wchar_t buffer[32];
    GetWindowTextW(edit, buffer, 32);
    return _wtoi(buffer);
}

bool get_check(HWND control) { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void set_check(HWND control, bool checked) {
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

int combo_index(HWND combo) {
    return combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
}

void combo_set_index(HWND combo, int index) {
    if (combo) {
        SendMessageW(combo, CB_SETCURSEL, index, 0);
    }
}

LRESULT CALLBACK PanelForwardProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR subclass_id, DWORD_PTR reference_data) {
    if (message == WM_COMMAND) {
        SendMessageW(reinterpret_cast<HWND>(reference_data), WM_COMMAND, wparam, lparam);
        return 0;
    }
    if (message == WM_NOTIFY) {  // the result matters for custom drawing
        return SendMessageW(reinterpret_cast<HWND>(reference_data), WM_NOTIFY, wparam, lparam);
    }
    if (message == WM_DRAWITEM || message == WM_MEASUREITEM || message == WM_CTLCOLORSTATIC ||
        message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        return SendMessageW(reinterpret_cast<HWND>(reference_data), message, wparam, lparam);
    }
    if ((message == WM_VSCROLL || message == WM_MOUSEWHEEL) &&
        scroll_message(window, message, wparam)) {
        return 0;
    }
    // The page draws its cards; themed check boxes ask for this background too.
    if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT) {
        paint_panel(window, reinterpret_cast<HDC>(wparam));
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_panel(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, PanelForwardProc, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

bool copy_text_to_clipboard(HWND owner, const wchar_t* text) {
    const size_t character_count = wcslen(text) + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, character_count * sizeof(wchar_t));
    if (!memory) {
        return false;
    }
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    wcscpy_s(destination, character_count, text);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    const bool copied =
        EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) {
        GlobalFree(memory);
    }
    return copied;
}

} // namespace settings
} // namespace cxxime
