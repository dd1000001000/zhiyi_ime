// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_ABOUT_DIALOG_H_
#define CXXIME_TSF_ABOUT_DIALOG_H_

#include <cxxime/version.h>
#include <windows.h>

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline void show_about_dialog(HWND parent = nullptr) {
    HWND existing = FindWindowW(L"ZhiyiIMEAboutClass", nullptr);
    if (existing) {
        SetForegroundWindow(existing);
        return;
    }

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int w = 320, h = 220;
    int x = (sx - w) / 2, y = (sy - h) / 2;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZhiyiIMEAboutClass";
    RegisterClassExW(&wc);

    // Chinese for a Chinese Windows display language, English otherwise.
    const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ZhiyiIMEAboutClass", zh ? L"关于知意输入法" : L"About Zhiyi IME",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT hBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    auto label = [&](const wchar_t* text, int cy, int ch, HFONT font) {
        HWND h = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, cy, cw - 20, ch,
            hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    };

    label(zh ? L"知意输入法" : L"Zhiyi IME", 16, 24, hBold);
    label(zh ? L"版本 " CXXIME_VERSION_WSTRING L" · GPL-3.0"
             : L"Version " CXXIME_VERSION_WSTRING L" · GPL-3.0", 44, 20, hFont);
    label(zh ? L"轻量 · 开源 · 懂上文的中英文输入法"
             : L"Lightweight, open-source, context-aware Chinese and English input method",
          68, 20, hFont);
    label(zh ? L"基于 CxxIME 修改（Apache License 2.0），原项目："
             : L"Based on CxxIME (Apache License 2.0):", 96, 20, hFont);
    label(L"https://github.com/deanxyuan/cxx-ime", 120, 20, hFont);

    HWND hBtn = CreateWindowExW(0, L"BUTTON", zh ? L"确定" : L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        (cw - 80) / 2, ch - 36, 80, 26, hwnd, (HMENU)IDOK,
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(hFont);
    DeleteObject(hBold);
}

#endif // CXXIME_TSF_ABOUT_DIALOG_H_
