// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/diagnostic_log_path.h>

#include <string>

#include <windows.h>
#include <shlobj.h>

namespace cxxime {
namespace {

using GetCurrentPackageFamilyNameFn = LONG(WINAPI*)(UINT32*, PWSTR);

std::wstring current_package_family_name() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        return {};
    }

    auto get_package_family_name = reinterpret_cast<GetCurrentPackageFamilyNameFn>(
        GetProcAddress(kernel32, "GetCurrentPackageFamilyName"));
    if (!get_package_family_name) {
        return {};
    }

    UINT32 length = 0;
    if (get_package_family_name(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length <= 1) {
        return {};
    }

    std::wstring family_name(static_cast<size_t>(length), L'\0');
    if (get_package_family_name(&length, &family_name[0]) != ERROR_SUCCESS) {
        return {};
    }
    family_name.resize(static_cast<size_t>(length - 1));
    return family_name;
}

bool create_directory(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
        GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring resolve_diagnostic_log_directory() {
    PWSTR known_folder = nullptr;
    const std::wstring package_family = current_package_family_name();
    if (!package_family.empty()) {
        const DWORD flags = KF_FLAG_NO_PACKAGE_REDIRECTION | KF_FLAG_DONT_VERIFY;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, flags, nullptr, &known_folder)) &&
            known_folder) {
            std::wstring root(known_folder);
            CoTaskMemFree(known_folder);
            root += L"\\Packages\\";
            root += package_family;
            root += L"\\LocalState";
            if (!create_directory(root)) {
                return {};
            }
            root += L"\\zhiyi";
            if (!create_directory(root)) {
                return {};
            }
            root += L"\\logs";
            return create_directory(root) ? root : std::wstring();
        }
        CoTaskMemFree(known_folder);
        return {};
    }

    wchar_t profile[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile))) {
        return {};
    }

    std::wstring root(profile);
    root += L"\\zhiyi";
    if (!create_directory(root)) {
        return {};
    }
    root += L"\\logs";
    return create_directory(root) ? root : std::wstring();
}

bool resolve_diagnostic_log_directory_is_packaged() {
    return !current_package_family_name().empty();
}

} // namespace

std::wstring diagnostic_log_directory() {
    static const std::wstring directory = resolve_diagnostic_log_directory();
    return directory;
}

bool diagnostic_log_directory_is_packaged() {
    static const bool packaged = resolve_diagnostic_log_directory_is_packaged();
    return packaged;
}

} // namespace cxxime
