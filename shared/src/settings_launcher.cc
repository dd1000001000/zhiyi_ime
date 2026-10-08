// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/settings_launcher.h>

#include <vector>

#include <windows.h>

#include <cxxime/logging.h>

#include "settings_launcher_util.h"

namespace cxxime {
namespace {

constexpr wchar_t kInstallRegistryKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZhiyiIME";
constexpr wchar_t kInstallerMutexName[] = L"Global\\ZhiyiIME.Installation";

class InstallationExclusion {
public:
    InstallationExclusion() {
        SetLastError(ERROR_SUCCESS);
        handle_ = CreateMutexW(nullptr, FALSE, kInstallerMutexName);
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (handle_) {
                CloseHandle(handle_);
                handle_ = nullptr;
            }
        }
    }

    ~InstallationExclusion() {
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    InstallationExclusion(const InstallationExclusion&) = delete;
    InstallationExclusion& operator=(const InstallationExclusion&) = delete;

    explicit operator bool() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

std::wstring registered_settings_path() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallRegistryKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return {};
    }

    wchar_t install_path[MAX_PATH] = {};
    DWORD value_type = 0;
    DWORD value_bytes = sizeof(install_path);
    const LONG result = RegQueryValueExW(key, L"InstallLocation", nullptr, &value_type,
                                         reinterpret_cast<BYTE*>(install_path), &value_bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || value_type != REG_SZ || value_bytes > sizeof(install_path)) {
        return {};
    }

    std::wstring path;
    if (!build_registered_settings_path(install_path, value_bytes, &path)) {
        return {};
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    return path;
}

std::wstring process_image_path(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return {};
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    std::wstring path;
    for (;;) {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
            path.assign(buffer.data(), length);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    CloseHandle(process);
    return path;
}

HWND find_settings_window(const std::wstring& path) {
    HWND previous = nullptr;
    for (;;) {
        HWND window = FindWindowExW(nullptr, previous, kSettingsWindowClass, nullptr);
        if (!window) {
            return nullptr;
        }
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        const std::wstring image_path = process_image_path(process_id);
        if (!image_path.empty() && settings_paths_equal(image_path, path)) {
            return window;
        }
        previous = window;
    }
}

bool activate_existing_settings(const std::wstring& path, SettingsPanel panel) {
    HWND window = find_settings_window(path);
    if (!window) {
        return false;
    }

    // Settings brings itself forward on this message too (editor_app.cc), with the right passed on.
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != 0) AllowSetForegroundWindow(process_id);
    const UINT message = RegisterWindowMessageW(kSettingsNavigateMessage);
    if (message != 0) {
        PostMessageW(window, message, static_cast<WPARAM>(panel), 0);
    }
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    return true;
}

} // namespace

bool open_settings(SettingsPanel panel) {
    InstallationExclusion exclusion;
    if (!exclusion) {
        CXXIME_LOG(L"%s", L"settings_launch result=0 reason=installer_active");
        return false;
    }

    const std::wstring path = registered_settings_path();
    if (path.empty()) {
        CXXIME_LOG(L"%s", L"settings_launch result=0 reason=active_path_unavailable");
        return false;
    }
    if (activate_existing_settings(path, panel)) {
        return true;
    }

    std::wstring command_line;
    wchar_t* mutable_command_line = nullptr;
    if (panel == SettingsPanel::kDictionary) {
        command_line =
            L"\"" + path + L"\" " + kSettingsPanelArgument + L" " + kSettingsDictionaryArgument;
        mutable_command_line = &command_line[0];
    }

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessW(path.c_str(), mutable_command_line, nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup_info, &process_info)) {
        CXXIME_LOG(L"settings_launch result=0 error=%lu", GetLastError());
        return false;
    }
    // Lets the new window come to the front when this process may (tray menu, menu click).
    AllowSetForegroundWindow(process_info.dwProcessId);
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
    return true;
}

} // namespace cxxime
