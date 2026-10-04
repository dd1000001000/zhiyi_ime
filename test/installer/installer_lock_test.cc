// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <restartmanager.h>

#include <cxxime/installer_lock.h>

#include "support/testutil.h"

TEST(InstallerLock, missing_file_has_no_locks) {
    const auto result = cxxime::installer::query_file_locks({L"missing-zhiyi-installer-file"});
    ASSERT_EQ(result.status, cxxime::installer::LockQueryStatus::kSuccess);
    ASSERT_TRUE(result.applications.empty());
}

TEST(InstallerLock, reports_process_holding_file) {
    wchar_t temp_directory[MAX_PATH] = {};
    wchar_t temp_path[MAX_PATH] = {};
    ASSERT_TRUE(GetTempPathW(MAX_PATH, temp_directory) != 0);
    ASSERT_TRUE(GetTempFileNameW(temp_directory, L"cxi", 0, temp_path) != 0);

    HANDLE file = CreateFileW(temp_path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(file != INVALID_HANDLE_VALUE);

    const auto result = cxxime::installer::query_file_locks({temp_path});
    CloseHandle(file);
    DeleteFileW(temp_path);

    const DWORD process_id = GetCurrentProcessId();
    const auto current_process =
        std::find_if(result.applications.begin(), result.applications.end(),
                     [process_id](const cxxime::installer::LockingApplication& application) {
                         return application.process_id == process_id;
                     });
    // This test deliberately owns the resource, which Restart Manager may report as self.
    const bool detected_self =
        result.status == cxxime::installer::LockQueryStatus::kRebootRequired &&
        result.reboot_reasons == static_cast<std::uint32_t>(RmRebootReasonDetectedSelf);
    ASSERT_TRUE(result.status == cxxime::installer::LockQueryStatus::kSuccess || detected_self)
        << "status=" << result.status << " reboot_reasons=" << result.reboot_reasons;
    ASSERT_TRUE(current_process != result.applications.end());
}

TEST(InstallerLock, formats_bounded_application_list) {
    cxxime::installer::LockQueryResult result;
    result.applications = {
        {10, L"First"},
        {20, L"Second"},
        {30, L"Third"},
    };

    const std::wstring report = cxxime::installer::format_lock_report(result, 2);
    ASSERT_TRUE(report.find(L"First (PID 10)") != std::wstring::npos);
    ASSERT_TRUE(report.find(L"Second (PID 20)") != std::wstring::npos);
    ASSERT_TRUE(report.find(L"Third") == std::wstring::npos);
    ASSERT_TRUE(report.find(L"另外 1 个应用程序") != std::wstring::npos);
}

TEST(InstallerLock, truncates_long_application_names) {
    cxxime::installer::LockQueryResult result;
    result.applications = {{10, std::wstring(200, L'x')}};

    const std::wstring report = cxxime::installer::format_lock_report(result);
    ASSERT_TRUE(report.size() < 160);
    ASSERT_TRUE(report.find(L"...") != std::wstring::npos);
}

TEST(InstallerLock, formats_query_failure_in_chinese) {
    cxxime::installer::LockQueryResult result;
    result.status = cxxime::installer::LockQueryStatus::kFailed;
    result.error_code = 123;

    const std::wstring report = cxxime::installer::format_lock_report(result);
    ASSERT_TRUE(report.find(L"无法检查文件占用情况") != std::wstring::npos);
    ASSERT_TRUE(report.find(L"123") != std::wstring::npos);
}

RUN_ALL_TESTS()
