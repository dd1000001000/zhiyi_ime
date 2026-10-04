// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app_internal.h"

#include <algorithm>
#include <cwchar>

namespace cxxime {
namespace settings {

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
    kListW = S(150);
    kPadX = S(162);
    kPadY = S(16);
    kCtrlH = S(kFontPt + 14);
    kRowH = S(kFontPt + 20);
    kPanelPadTop = S(8);
    kPanelPadLeft = S(8);
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

void make_aligned_label(const wchar_t* text, int y, HWND parent) {
    HWND control =
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT, kPanelPadLeft, y,
                        kLblW, kCtrlH, parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
}

int make_aligned_label(const wchar_t* text, int x, int width, int y, HWND parent) {
    HWND control =
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT, x, y, width, kCtrlH,
                        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return x + width + S(8);
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

HWND make_combo(int id, int x, int y, int width, HWND parent) {
    HWND combo =
        CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                        x, y, width, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        GetModuleHandle(nullptr), nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
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
    if (message == WM_NOTIFY) {
        SendMessageW(reinterpret_cast<HWND>(reference_data), WM_NOTIFY, wparam, lparam);
        return 0;
    }
    if (message == WM_DRAWITEM || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
        return SendMessageW(reinterpret_cast<HWND>(reference_data), message, wparam, lparam);
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
