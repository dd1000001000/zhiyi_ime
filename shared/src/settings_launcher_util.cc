// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "settings_launcher_util.h"

#include <vector>

#include <windows.h>

namespace cxxime {
namespace {

bool is_drive_absolute_path(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    const wchar_t drive = path[0];
    return (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
}

std::wstring absolute_path(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        return {};
    }
    return std::wstring(buffer.data(), written);
}

} // namespace

bool build_registered_settings_path(const wchar_t* value, std::size_t value_bytes,
                                    std::wstring* settings_path) {
    if (!value || !settings_path) {
        return false;
    }
    settings_path->clear();
    if (value_bytes < sizeof(wchar_t) || value_bytes % sizeof(wchar_t) != 0) {
        return false;
    }

    const std::size_t path_chars = value_bytes / sizeof(wchar_t);
    if (value[path_chars - 1] != L'\0') {
        return false;
    }
    for (std::size_t i = 0; i + 1 < path_chars; ++i) {
        if (value[i] == L'\0') {
            return false;
        }
    }

    std::wstring path(value, path_chars - 1);
    if (!is_drive_absolute_path(path)) {
        return false;
    }
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    if (path.empty()) {
        return false;
    }

    constexpr wchar_t kSettingsExecutable[] = L"\\zhiyi-settings.exe";
    if (path.size() + _countof(kSettingsExecutable) > MAX_PATH) {
        return false;
    }
    *settings_path = path + kSettingsExecutable;
    return true;
}

bool settings_paths_equal(const std::wstring& left, const std::wstring& right) {
    const std::wstring absolute_left = absolute_path(left);
    const std::wstring absolute_right = absolute_path(right);
    if (absolute_left.empty() || absolute_right.empty()) {
        return false;
    }
    return CompareStringOrdinal(absolute_left.c_str(), static_cast<int>(absolute_left.size()),
                                absolute_right.c_str(), static_cast<int>(absolute_right.size()),
                                TRUE) == CSTR_EQUAL;
}

} // namespace cxxime
