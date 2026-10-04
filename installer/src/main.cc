// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include <cxxime/installer_lock.h>
#include <cxxime/installer_path_security.h>
#include <cxxime/installer_server_process.h>
#include <cxxime/installer_tsf.h>
#include <cxxime/installer_ui_language.h>
#include <cxxime/installer_version.h>

#include "installer_lifecycle_command.h"

namespace {

constexpr int kExitNoLocks = 0;
constexpr int kExitLocked = 2;
constexpr int kExitRebootRequired = 3;
constexpr int kExitQueryFailed = 4;
constexpr int kExitLockedAndRebootRequired = 5;
constexpr int kExitInvalidArguments = 64;

bool write_utf16_report(const std::wstring& path, const std::wstring& report) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::uint16_t bom = 0xfeff;
    DWORD written = 0;
    bool success =
        WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE && written == sizeof(bom);
    if (success && !report.empty()) {
        const DWORD byte_count = static_cast<DWORD>(report.size() * sizeof(wchar_t));
        success = WriteFile(file, report.data(), byte_count, &written, nullptr) != FALSE &&
                  written == byte_count;
    }
    CloseHandle(file);
    return success;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"get-ui-language") {
        return cxxime::installer::get_ui_language_command(argv[2]);
    }
    if (argc == 4 && std::wstring(argv[1]) == L"set-ui-language") {
        return cxxime::installer::set_ui_language_command(argv[2], argv[3]);
    }
    if (argc == 4 && std::wstring(argv[1]) == L"compare-version") {
        return cxxime::installer::compare_version_command(argv[2], argv[3]);
    }
    if (argc == 6 && std::wstring(argv[1]) == L"lifecycle-prepare") {
        return cxxime::installer::prepare_install_lifecycle_command(argv[2], argv[3], argv[4],
                                                                   argv[5]);
    }
    if (argc == 5 && std::wstring(argv[1]) == L"lifecycle-commit") {
        return cxxime::installer::commit_install_lifecycle_command(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && std::wstring(argv[1]) == L"lifecycle-gc") {
        return cxxime::installer::collect_install_garbage_command(argv[2], argv[3]);
    }
    if (argc == 4 && std::wstring(argv[1]) == L"lifecycle-prepared-target") {
        return cxxime::installer::prepared_install_target_command(argv[2], argv[3]);
    }
    if (argc == 5 && std::wstring(argv[1]) == L"lifecycle-uninstall") {
        return cxxime::installer::uninstall_lifecycle_command(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && std::wstring(argv[1]) == L"lifecycle-validate-uninstall") {
        return cxxime::installer::validate_uninstall_lifecycle_command(argv[2], argv[3]);
    }
    if (argc == 2 && std::wstring(argv[1]) == L"release") {
        return cxxime::installer::release_input_processor_with_timeout();
    }
    if (argc == 2 && std::wstring(argv[1]) == L"release-worker") {
        return cxxime::installer::release_input_processor();
    }
    if (argc == 3 && std::wstring(argv[1]) == L"server-running") {
        return cxxime::installer::server_running(argv[2]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"server-ready") {
        return cxxime::installer::server_ready(argv[2]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"secure-install-root") {
        return cxxime::installer::secure_install_root(argv[2]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"validate-install-directory") {
        return cxxime::installer::validate_install_directory(argv[2]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"server-pid") {
        return cxxime::installer::print_server_pid(argv[2]);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"stop-server") {
        return cxxime::installer::stop_server(argv[2], false);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"force-stop-server") {
        return cxxime::installer::stop_server(argv[2], true);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"start-server") {
        return cxxime::installer::start_server(argv[2]);
    }
    if (argc < 5 || std::wstring(argv[1]) != L"query" || std::wstring(argv[2]) != L"--report") {
        return kExitInvalidArguments;
    }

    const std::wstring report_path = argv[3];
    std::vector<std::wstring> resources;
    for (int index = 4; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument.compare(0, 2, L"--") == 0) {
            return kExitInvalidArguments;
        }
        resources.push_back(argument);
    }

    const auto result = cxxime::installer::query_file_locks(resources);
    const std::wstring report = cxxime::installer::format_lock_report(result);
    if (!write_utf16_report(report_path, report)) {
        return kExitQueryFailed;
    }
    if (result.status == cxxime::installer::LockQueryStatus::kFailed) {
        return kExitQueryFailed;
    }
    if (result.status == cxxime::installer::LockQueryStatus::kRebootRequired) {
        return result.applications.empty() ? kExitRebootRequired : kExitLockedAndRebootRequired;
    }
    return result.applications.empty() ? kExitNoLocks : kExitLocked;
}
