// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"
#include <shellscalingapi.h>
#include <shellapi.h>
#include <cxxime/data_path.h>
#include <cxxime/settings_route.h>

#pragma comment(lib, "shcore.lib")

static float get_dpi_scale() {
    HDC dc = GetDC(nullptr);
    float s = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
    ReleaseDC(nullptr, dc);
    return s;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    float dpiScale = get_dpi_scale();

    cxxime::SettingsPanel initialPanel = cxxime::SettingsPanel::kInput;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], cxxime::kSettingsPanelArgument) == 0 && i + 1 < argc) {
                if (wcscmp(argv[i + 1], cxxime::kSettingsDictionaryArgument) == 0) {
                    initialPanel = cxxime::SettingsPanel::kDictionary;
                }
                ++i;
            } else if (wcscmp(argv[i], L"--data") == 0 && i + 1 < argc) {
                std::string dir;
                int len = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1,
                                              nullptr, 0, nullptr, nullptr);
                if (len > 1) {
                    dir.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1,
                                       &dir[0], len, nullptr, nullptr);
                }
                cxxime::set_data_dir(dir);
                ++i;
            }
        }
        LocalFree(argv);
    }

    // One settings window: a second start (e.g. the installer's finish page after an update,
    // which also opens settings itself) brings the open one forward instead.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\ZhiyiIME.Settings.Instance");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int attempt = 0; attempt < 30; ++attempt) {
            HWND existing = FindWindowW(cxxime::kSettingsWindowClass, nullptr);
            if (existing) {
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
                if (initialPanel != cxxime::SettingsPanel::kInput) {
                    PostMessageW(existing, RegisterWindowMessageW(cxxime::kSettingsNavigateMessage),
                                 static_cast<WPARAM>(initialPanel), 0);
                }
                break;
            }
            Sleep(100);  // still starting
        }
        CloseHandle(instance);
        return 0;
    }
    const int result = cxxime::settings::EditorApp::run(hInst, dpiScale, initialPanel);
    if (instance) CloseHandle(instance);
    return result;
}
