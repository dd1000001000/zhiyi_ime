// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// status_window_tool — interactive visual test for StatusWindow redesign

#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <string>
#include <cxxime/status_window.h>
#include <cxxime/render_context.h>
#include <cxxime/config.h>
#include <cxxime/data_path.h>

static cxxime::StatusWindow g_window;
static cxxime::ButtonState g_state;
static cxxime::StatusTheme g_theme;
static bool g_enabled = true;
static HWND g_parent = nullptr;

// ── Helpers ──────────────────────────────────────────────────
static const wchar_t* input_mode_text() {
    switch (g_state.input_mode) {
    case cxxime::InputMode::PINYIN:
        return L"拼";
    case cxxime::InputMode::WUBI:
        return L"五";
    case cxxime::InputMode::MIXED:
        return L"混";
    }
    return L"拼";
}

static void cycle_input_mode() {
    switch (g_state.input_mode) {
    case cxxime::InputMode::PINYIN:
        g_state.input_mode = cxxime::InputMode::WUBI;
        break;
    case cxxime::InputMode::WUBI:
        g_state.input_mode = cxxime::InputMode::MIXED;
        break;
    case cxxime::InputMode::MIXED:
        g_state.input_mode = cxxime::InputMode::PINYIN;
        break;
    }
}

static void update_display() {
    g_window.update_state(g_state);
    g_window.show();

    wchar_t title[256];
    swprintf(title, 256,
        L"模式:%ls 中:%ls 全:%ls 。:%ls %ls",
        input_mode_text(),
        g_state.chinese_mode  ? L"中" : L"英",
        g_state.full_shape    ? L"全" : L"半",
        g_state.chinese_punct ? L"。": L".",
        g_enabled ? L"" : L"(DISABLED)");
    if (g_parent) SetWindowTextW(g_parent, title);
}

static void print_state() {
    wprintf(L"状态: 模式=%ls  中=%ls  全=%ls  。=%ls  %ls\n",
            input_mode_text(),
            g_state.chinese_mode ? L"中" : L"英",
            g_state.full_shape ? L"全" : L"半",
            g_state.chinese_punct ? L"。" : L".",
            g_enabled ? L"" : L"[IPC断开]");
}

// ── Click callback ───────────────────────────────────────────
static void on_button_click(cxxime::StatusButton btn) {
    switch (btn) {
    case cxxime::StatusButton::CHINESE_MODE:
        g_state.chinese_mode = !g_state.chinese_mode;
        wprintf(L"→ 切换: %ls\n", g_state.chinese_mode ? L"中文模式" : L"英文模式");
        break;
    case cxxime::StatusButton::FULL_SHAPE:
        g_state.full_shape = !g_state.full_shape;
        wprintf(L"→ 切换: %ls\n", g_state.full_shape ? L"全角" : L"半角");
        break;
    case cxxime::StatusButton::CHINESE_PUNCT:
        g_state.chinese_punct = !g_state.chinese_punct;
        wprintf(L"→ 切换: %ls\n", g_state.chinese_punct ? L"中文标点" : L"英文标点");
        break;
    case cxxime::StatusButton::SETTINGS:
        wprintf(L"→ 设置按键被点击 (在正式版本中会启动设置程序)\n");
        break;
    }
    update_display();
}

// ── Window procedure ─────────────────────────────────────────
static LRESULT CALLBACK ParentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        int vk = (int)wp;
        switch (vk) {
        case '1':
            g_state.chinese_mode = !g_state.chinese_mode;
            print_state(); update_display(); break;
        case '2':
            g_state.full_shape = !g_state.full_shape;
            print_state(); update_display(); break;
        case '3':
            g_state.chinese_punct = !g_state.chinese_punct;
            print_state(); update_display(); break;
        case 'M':
            cycle_input_mode();
            print_state(); update_display(); break;
        case 'E':
            g_enabled = !g_enabled;
            g_window.set_enabled(g_enabled);
            wprintf(L"→ IPC状态: %ls\n", g_enabled ? L"已连接" : L"已断开");
            print_state(); update_display(); break;
        case VK_LEFT: {
            int x, y; g_window.get_position(x, y);
            g_window.set_position(x - 10, y);
            wprintf(L"→ 位置: (%d, %d)\n", x - 10, y); break;
        }
        case VK_RIGHT: {
            int x, y; g_window.get_position(x, y);
            g_window.set_position(x + 10, y);
            wprintf(L"→ 位置: (%d, %d)\n", x + 10, y); break;
        }
        case VK_UP: {
            int x, y; g_window.get_position(x, y);
            g_window.set_position(x, y - 10);
            wprintf(L"→ 位置: (%d, %d)\n", x, y - 10); break;
        }
        case VK_DOWN: {
            int x, y; g_window.get_position(x, y);
            g_window.set_position(x, y + 10);
            wprintf(L"→ 位置: (%d, %d)\n", x, y + 10); break;
        }
        case VK_ESCAPE:
            wprintf(L"退出\n");
            PostQuitMessage(0); break;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry point ──────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc > 1) {
        // A color scheme from data/themes.json, e.g. moon_dark.
        cxxime::Config config;
        config.load_themes(cxxime::data_path("themes.json"));
        config.theme = argv[1];
        g_theme = cxxime::build_status_theme_from_config(config);
    }
    wprintf(L"=== Zhiyi IME Status Window Tool ===\n");
    wprintf(L"Keys:  1     = Toggle 中/英\n");
    wprintf(L"       2     = Toggle 全/半\n");
    wprintf(L"       3     = Toggle 。/.\n");
    wprintf(L"       M     = Cycle 拼/五/混\n");
    wprintf(L"       E     = Toggle IPC connected/disconnected\n");
    wprintf(L"       Arrow = Move window\n");
    wprintf(L"       Esc   = Exit\n\n");
    wprintf(L"Click on buttons to toggle state.\n\n");

    // Create parent window (keyboard focus)
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ParentWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"ZhiyiIMEStatusToolParent";
    RegisterClassExW(&wc);

    g_parent = CreateWindowExW(0, L"ZhiyiIMEStatusToolParent",
        L"Status Window Tool",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 120,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    // Create status window
    g_window.create(g_theme);
    g_window.set_click_callback(on_button_click);

    // Initial display
    update_display();
    print_state();
    ShowWindow(g_parent, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_window.destroy();
    return 0;
}
