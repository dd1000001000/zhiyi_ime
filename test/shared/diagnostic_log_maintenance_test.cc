// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdint>
#include <cwchar>
#include <string>

#include <windows.h>

#include <cxxime/diagnostic_log_maintenance.h>

#include "support/testutil.h"

namespace {

class TempDirectory {
public:
    TempDirectory() {
        wchar_t root[MAX_PATH] = {};
        wchar_t temporary[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, root) && GetTempFileNameW(root, L"cxl", 0, temporary)) {
            DeleteFileW(temporary);
            if (CreateDirectoryW(temporary, nullptr)) {
                path_ = temporary;
            }
        }
    }

    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        WIN32_FIND_DATAW find_data = {};
        HANDLE find = FindFirstFileW((path_ + L"\\*").c_str(), &find_data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(find_data.cFileName, L".") != 0 &&
                    wcscmp(find_data.cFileName, L"..") != 0) {
                    DeleteFileW((path_ + L"\\" + find_data.cFileName).c_str());
                }
            } while (FindNextFileW(find, &find_data));
            FindClose(find);
        }
        RemoveDirectoryW(path_.c_str());
    }

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

std::wstring create_file(const std::wstring& directory, const std::wstring& name,
                         std::uint32_t size) {
    const std::wstring path = directory + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string content(size, 'x');
    DWORD written = 0;
    WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(file);
    return path;
}

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void set_age(const std::wstring& path, std::uint64_t age_100ns) {
    HANDLE file =
        CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(file != INVALID_HANDLE_VALUE);
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER value = {};
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    value.QuadPart -= age_100ns;
    FILETIME last_write = {};
    last_write.dwLowDateTime = value.LowPart;
    last_write.dwHighDateTime = value.HighPart;
    ASSERT_TRUE(SetFileTime(file, nullptr, nullptr, &last_write));
    CloseHandle(file);
}

} // namespace

TEST(DiagnosticLogMaintenance, accepts_only_owned_log_names) {
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"server-trace.jsonl"));
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"server-trace.jsonl.4"));
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"tsf-1234-trace.jsonl"));
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"tsf-1234-trace.jsonl.2"));
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"host-tsf-1234-x64.jsonl"));
    ASSERT_TRUE(cxxime::is_diagnostic_log_filename(L"host-tsf-1234-x64.jsonl.1"));
    ASSERT_TRUE(!cxxime::is_diagnostic_log_filename(L"notes.jsonl"));
    ASSERT_TRUE(!cxxime::is_diagnostic_log_filename(L"server-trace.jsonl.old"));
    ASSERT_TRUE(!cxxime::is_diagnostic_log_filename(L"tsf-name-trace.jsonl"));
    ASSERT_TRUE(!cxxime::is_diagnostic_log_filename(L"host-.jsonl"));
}

TEST(DiagnosticLogMaintenance, purge_removes_logs_and_preserves_other_files) {
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring server = create_file(directory.path(), L"server-trace.jsonl", 10);
    const std::wstring tsf = create_file(directory.path(), L"tsf-42-trace.jsonl.1", 20);
    const std::wstring unrelated = create_file(directory.path(), L"notes.jsonl", 30);

    const auto result = cxxime::cleanup_diagnostic_log_directory(
        directory.path(), cxxime::diagnostic_log_purge_options());
    ASSERT_TRUE(result.maintenance_performed);
    ASSERT_EQ(result.deleted_files, 2U);
    ASSERT_EQ(result.deleted_bytes, 30ULL);
    ASSERT_TRUE(!exists(server));
    ASSERT_TRUE(!exists(tsf));
    ASSERT_TRUE(exists(unrelated));
    ASSERT_TRUE(exists(directory.path() + L"\\.zhiyi-log-maintenance"));
}

TEST(DiagnosticLogMaintenance, retention_deletes_oldest_to_low_watermark) {
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring first = create_file(directory.path(), L"tsf-1-trace.jsonl", 40);
    Sleep(20);
    const std::wstring second = create_file(directory.path(), L"tsf-2-trace.jsonl", 40);
    Sleep(20);
    const std::wstring third = create_file(directory.path(), L"server-trace.jsonl", 40);

    cxxime::DiagnosticLogCleanupOptions options;
    options.mode = cxxime::DiagnosticLogCleanupMode::kRetention;
    options.high_watermark = 100;
    options.low_watermark = 75;
    const auto result = cxxime::cleanup_diagnostic_log_directory(directory.path(), options);
    ASSERT_TRUE(result.maintenance_performed);
    ASSERT_EQ(result.deleted_files, 2U);
    ASSERT_TRUE(!exists(first));
    ASSERT_TRUE(!exists(second));
    ASSERT_TRUE(exists(third));
}

TEST(DiagnosticLogMaintenance, retention_deletes_expired_logs_and_throttles_repeat_scan) {
    constexpr std::uint64_t kDay = 24ULL * 60ULL * 60ULL * 10000000ULL;
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring expired = create_file(directory.path(), L"tsf-10-trace.jsonl", 10);
    const std::wstring current = create_file(directory.path(), L"tsf-11-trace.jsonl", 10);
    set_age(expired, 8ULL * kDay);

    cxxime::DiagnosticLogCleanupOptions options;
    options.mode = cxxime::DiagnosticLogCleanupMode::kRetention;
    options.max_age_100ns = 7ULL * kDay;
    options.minimum_interval_100ns = kDay;
    const auto result = cxxime::cleanup_diagnostic_log_directory(directory.path(), options);
    ASSERT_EQ(result.deleted_files, 1U);
    ASSERT_TRUE(!exists(expired));
    ASSERT_TRUE(exists(current));

    const auto repeat = cxxime::cleanup_diagnostic_log_directory(directory.path(), options);
    ASSERT_TRUE(repeat.throttled);
    ASSERT_TRUE(!repeat.maintenance_performed);
}

TEST(DiagnosticLogMaintenance, future_lock_timestamp_does_not_throttle_cleanup) {
    constexpr std::uint64_t kDay = 24ULL * 60ULL * 60ULL * 10000000ULL;
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring log = create_file(directory.path(), L"server-trace.jsonl", 10);
    const std::wstring lock = create_file(directory.path(), L".zhiyi-log-maintenance", 0);
    ASSERT_TRUE(!lock.empty());

    HANDLE file =
        CreateFileW(lock.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(file != INVALID_HANDLE_VALUE);
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER future = {};
    future.LowPart = now.dwLowDateTime;
    future.HighPart = now.dwHighDateTime;
    future.QuadPart += kDay;
    FILETIME last_write = {};
    last_write.dwLowDateTime = future.LowPart;
    last_write.dwHighDateTime = future.HighPart;
    ASSERT_TRUE(SetFileTime(file, nullptr, nullptr, &last_write));
    CloseHandle(file);

    cxxime::DiagnosticLogCleanupOptions options;
    options.mode = cxxime::DiagnosticLogCleanupMode::kRetention;
    options.high_watermark = 1;
    options.low_watermark = 0;
    options.minimum_interval_100ns = kDay;
    const auto result = cxxime::cleanup_diagnostic_log_directory(directory.path(), options);
    ASSERT_TRUE(result.maintenance_performed);
    ASSERT_TRUE(!result.throttled);
    ASSERT_TRUE(!exists(log));
}

TEST(DiagnosticLogMaintenance, locked_log_is_skipped_without_blocking) {
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring path = create_file(directory.path(), L"server-trace.jsonl", 10);
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(held != INVALID_HANDLE_VALUE);

    const auto result = cxxime::cleanup_diagnostic_log_directory(
        directory.path(), cxxime::diagnostic_log_purge_options());
    ASSERT_EQ(result.deleted_files, 0U);
    ASSERT_EQ(result.skipped_files, 1U);
    ASSERT_TRUE(exists(path));
    CloseHandle(held);
}

TEST(DiagnosticLogMaintenance, maintenance_lock_is_cross_process_style_exclusive) {
    TempDirectory directory;
    ASSERT_TRUE(!directory.path().empty());
    const std::wstring lock_path = directory.path() + L"\\.zhiyi-log-maintenance";
    HANDLE held = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    ASSERT_TRUE(held != INVALID_HANDLE_VALUE);

    const auto result = cxxime::cleanup_diagnostic_log_directory(
        directory.path(), cxxime::diagnostic_log_purge_options());
    ASSERT_TRUE(result.lock_busy);
    ASSERT_TRUE(!result.maintenance_performed);
    CloseHandle(held);

    const auto retry = cxxime::cleanup_diagnostic_log_directory(
        directory.path(), cxxime::diagnostic_log_purge_options());
    ASSERT_TRUE(retry.maintenance_performed);
}

RUN_ALL_TESTS()
