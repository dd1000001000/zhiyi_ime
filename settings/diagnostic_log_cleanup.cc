// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "diagnostic_log_cleanup.h"

#include <cwchar>
#include <string>

#include <windows.h>
#include <shlobj.h>

#include <cxxime/diagnostic_log_maintenance.h>
#include <cxxime/diagnostic_log_path.h>

namespace cxxime {
namespace settings {
namespace {

void add_cleanup_result(const cxxime::DiagnosticLogCleanupResult& result,
                        DiagnosticsCleanupSummary* summary) {
    if (result.directory_found) {
        ++summary->directories;
    }
        summary->deleted_files += result.deleted_files;
        summary->skipped_files += result.skipped_files;
        summary->deleted_bytes += result.deleted_bytes;
    if (result.lock_busy || (!result.maintenance_performed && !result.throttled &&
            result.error_code != ERROR_SUCCESS)) {
        ++summary->inaccessible_directories;
    }
}

void cleanup_directory(const std::wstring& directory, DiagnosticsCleanupSummary* summary) {
    add_cleanup_result(
        cxxime::cleanup_diagnostic_log_directory(directory, cxxime::diagnostic_log_purge_options()),
        summary);
}

void cleanup_packaged_app_directories(DiagnosticsCleanupSummary* summary) {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
                                    &local_app_data)) ||
        !local_app_data) {
        ++summary->inaccessible_directories;
        CoTaskMemFree(local_app_data);
        return;
    }

    const std::wstring packages = std::wstring(local_app_data) + L"\\Packages";
    CoTaskMemFree(local_app_data);
    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW((packages + L"\\*").c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            ++summary->inaccessible_directories;
        }
        return;
    }

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring directory =
            packages + L"\\" + find_data.cFileName + L"\\LocalState\\zhiyi\\logs";
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            cleanup_directory(directory, summary);
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
                ++summary->inaccessible_directories;
            }
        }
    } while (FindNextFileW(find, &find_data));
    const DWORD enumeration_error = GetLastError();
    FindClose(find);
    if (enumeration_error != ERROR_NO_MORE_FILES) {
        ++summary->inaccessible_directories;
    }
}

} // namespace

DiagnosticsCleanupSummary cleanup_current_user_diagnostic_logs() {
    DiagnosticsCleanupSummary summary;
    cleanup_directory(cxxime::diagnostic_log_directory(), &summary);
    cleanup_packaged_app_directories(&summary);
    return summary;
}

} // namespace settings
} // namespace cxxime
