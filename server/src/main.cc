// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "server_app.h"

#include <string>

#include <objbase.h>
#include <shellapi.h>

#include <cxxime/config.h>
#include <cxxime/data_path.h>
#include <cxxime/query_trace.h>
#include <cxxime/server_launcher.h>

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

static std::string get_arg(int argc, LPWSTR* argv, const std::wstring& flag) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == flag)
            return wide_to_utf8(argv[i + 1]);
    }
    return {};
}

static bool has_flag(int argc, LPWSTR* argv, const std::wstring& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag)
            return true;
    }
    return false;
}

// startup.autostart is off: the sign-in Run entry (--autostart) does not start the server; the
// input method starts it when it is switched to.
static bool autostart_disabled(const std::string& config_path) {
    cxxime::Config config;
    if (!config.load(config_path.empty() ? cxxime::data_path("default.json") : config_path))
        return false;
    config.load_user(cxxime::user_data_path("default.json"));
    return !config.autostart;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        return 1;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::string dict_path;
    std::string config_path;
    bool autostart = false;
    if (argv) {
        // --data sets base data directory (overrides compile-time/default path)
        std::string data_dir = get_arg(argc, argv, L"--data");
        if (!data_dir.empty())
            cxxime::set_data_dir(data_dir);

        dict_path = get_arg(argc, argv, L"--dict");
        config_path = get_arg(argc, argv, L"--config");
        autostart = has_flag(argc, argv, L"--autostart");
        LocalFree(argv);
    }

    // Settings is moving the data folder: load nothing until it is done (the input method may
    // start the server meanwhile from any program).
    if (HANDLE moving = OpenMutexW(SYNCHRONIZE, FALSE, cxxime::kDataMoveMutex)) {
        const DWORD waited = WaitForSingleObject(moving, INFINITE);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) ReleaseMutex(moving);
        CloseHandle(moving);
    }

    // One server per sign-in session: the input method may start it from several programs at
    // once (server_launcher.h).
    HANDLE instance = CreateMutexW(nullptr, TRUE, cxxime::kServerInstanceMutex);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS ||
        (autostart && autostart_disabled(config_path))) {
        if (instance)
            CloseHandle(instance);
        CoUninitialize();
        return 0;
    }

    ServerApp app;
    if (!app.initialize(dict_path, config_path)) {
        CloseHandle(instance);
        CoUninitialize();
        return 1;
    }
    app.run();
    app.finalize();

    // Shutdown async trace writer (flush remaining entries)
    cxxime::QueryTrace::shutdown();
    CloseHandle(instance);
    CoUninitialize();

    return 0;
}
