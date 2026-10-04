// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/diagnostic_log_maintenance.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include <windows.h>

namespace cxxime {
namespace {

constexpr wchar_t kMaintenanceFilename[] = L".zhiyi-log-maintenance";
constexpr std::uint64_t kFileTimeTicksPerSecond = 10000000ULL;
constexpr std::uint64_t kRetentionAge = 7ULL * 24ULL * 60ULL * 60ULL * kFileTimeTicksPerSecond;
constexpr std::uint64_t kMaintenanceInterval = 6ULL * 60ULL * 60ULL * kFileTimeTicksPerSecond;

struct LogFile {
    std::wstring path;
    std::uint64_t size = 0;
    std::uint64_t last_write = 0;
    bool deletion_attempted = false;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle)
        : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint64_t file_time_value(const FILETIME& value) {
    ULARGE_INTEGER combined = {};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

std::uint64_t current_file_time() {
    FILETIME value = {};
    GetSystemTimeAsFileTime(&value);
    return file_time_value(value);
}

bool is_decimal(const std::wstring& value) {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

bool has_numeric_rotation(const std::wstring& filename, const std::wstring& base) {
    if (filename == base) {
        return true;
    }
    if (filename.size() <= base.size() + 1 || filename.compare(0, base.size(), base) != 0 ||
        filename[base.size()] != L'.') {
        return false;
    }
    return is_decimal(filename.substr(base.size() + 1));
}

bool is_host_trace_filename(const std::wstring& filename) {
    constexpr wchar_t kPrefix[] = L"host-";
    constexpr wchar_t kSuffix[] = L".jsonl";
    if (filename.compare(0, 5, kPrefix) != 0) {
        return false;
    }

    const size_t suffix = filename.rfind(kSuffix);
    if (suffix == std::wstring::npos || suffix == 5) {
        return false;
    }
    const std::wstring rotation = filename.substr(suffix + 6);
    if (!rotation.empty() && rotation != L".1") {
        return false;
    }
    return std::all_of(filename.begin() + 5, filename.begin() + suffix, [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
    });
}

bool delete_log_file(LogFile* file, DiagnosticLogCleanupResult* result) {
    file->deletion_attempted = true;
    if (!DeleteFileW(file->path.c_str())) {
        ++result->skipped_files;
        if (result->error_code == ERROR_SUCCESS) {
            result->error_code = GetLastError();
        }
        return false;
    }
    ++result->deleted_files;
    result->deleted_bytes += file->size;
    return true;
}

bool enumerate_log_files(const std::wstring& directory, std::vector<LogFile>* files,
                         DiagnosticLogCleanupResult* result) {
    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        result->error_code = error;
        return false;
    }

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !is_diagnostic_log_filename(find_data.cFileName)) {
            continue;
        }
        ULARGE_INTEGER size = {};
        size.LowPart = find_data.nFileSizeLow;
        size.HighPart = find_data.nFileSizeHigh;
        files->push_back({directory + L"\\" + find_data.cFileName, size.QuadPart,
                          file_time_value(find_data.ftLastWriteTime), false});
    } while (FindNextFileW(find, &find_data));

    const DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        result->error_code = error;
        return false;
    }
    result->matching_files = static_cast<std::uint32_t>(files->size());
    return true;
}

} // namespace

bool is_diagnostic_log_filename(const std::wstring& filename) {
    std::wstring lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

    if (has_numeric_rotation(lower, L"server-trace.jsonl")) {
        return true;
    }
    constexpr wchar_t kTsfPrefix[] = L"tsf-";
    constexpr wchar_t kTsfSuffix[] = L"-trace.jsonl";
    if (lower.compare(0, 4, kTsfPrefix) == 0) {
        const size_t suffix = lower.find(kTsfSuffix, 4);
        if (suffix != std::wstring::npos && is_decimal(lower.substr(4, suffix - 4)) &&
            has_numeric_rotation(lower, lower.substr(0, suffix + 12))) {
            return true;
        }
    }
    return is_host_trace_filename(lower);
}

DiagnosticLogCleanupOptions diagnostic_log_retention_options(const DiagnosticsConfig& config) {
    DiagnosticLogCleanupOptions options;
    options.mode = DiagnosticLogCleanupMode::kRetention;
    options.max_age_100ns = kRetentionAge;
    options.high_watermark = static_cast<std::uint64_t>(config.log_max_size) *
                             static_cast<std::uint64_t>(config.log_max_files + 1) * 2ULL;
    options.low_watermark = options.high_watermark * 3ULL / 4ULL;
    options.minimum_interval_100ns = kMaintenanceInterval;
    return options;
}

DiagnosticLogCleanupOptions diagnostic_log_purge_options() {
    DiagnosticLogCleanupOptions options;
    options.mode = DiagnosticLogCleanupMode::kPurgeHistory;
    return options;
}

DiagnosticLogCleanupResult
cleanup_diagnostic_log_directory(const std::wstring& directory,
                                 const DiagnosticLogCleanupOptions& options) {
    DiagnosticLogCleanupResult result;
    if (directory.empty()) {
        result.error_code = ERROR_INVALID_PARAMETER;
        return result;
    }

    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.error_code = GetLastError();
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.error_code = ERROR_DIRECTORY;
        return result;
    }
    result.directory_found = true;

    const std::wstring lock_path = directory + L"\\" + kMaintenanceFilename;
    HANDLE raw_lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    const DWORD open_error = GetLastError();
    ScopedHandle lock(raw_lock);
    if (raw_lock == INVALID_HANDLE_VALUE) {
        result.error_code = open_error;
        result.lock_busy =
            open_error == ERROR_SHARING_VIOLATION || open_error == ERROR_LOCK_VIOLATION;
        return result;
    }

    const bool existing_lock = open_error == ERROR_ALREADY_EXISTS;
    const std::uint64_t now = current_file_time();
    if (!existing_lock) {
        FILETIME never_completed = {};
        SetFileTime(lock.get(), nullptr, nullptr, &never_completed);
    }
    if (options.mode == DiagnosticLogCleanupMode::kRetention && existing_lock &&
        options.minimum_interval_100ns > 0) {
        FILETIME last_write = {};
        if (GetFileTime(lock.get(), nullptr, nullptr, &last_write)) {
            const std::uint64_t previous = file_time_value(last_write);
            if (previous <= now && now - previous < options.minimum_interval_100ns) {
                result.throttled = true;
                return result;
            }
        }
    }

    std::vector<LogFile> files;
    if (!enumerate_log_files(directory, &files, &result)) {
        return result;
    }
    result.maintenance_performed = true;

    if (options.mode == DiagnosticLogCleanupMode::kPurgeHistory) {
        for (auto& file : files) {
            delete_log_file(&file, &result);
        }
        return result;
    }

    std::uint64_t total_size = 0;
    for (auto& file : files) {
        total_size += file.size;
        if (options.max_age_100ns == 0 || file.last_write > now ||
            now - file.last_write <= options.max_age_100ns) {
            continue;
        }
        if (delete_log_file(&file, &result)) {
            total_size -= file.size;
        }
    }

    if (options.high_watermark > 0 && total_size > options.high_watermark) {
        std::sort(files.begin(), files.end(), [](const LogFile& left, const LogFile& right) {
            return left.last_write < right.last_write;
        });
        for (auto& file : files) {
            if (total_size <= options.low_watermark) {
                break;
            }
            if (!file.deletion_attempted && delete_log_file(&file, &result)) {
                total_size -= file.size;
            }
        }
    }

    FILETIME completed = {};
    GetSystemTimeAsFileTime(&completed);
    SetFileTime(lock.get(), nullptr, nullptr, &completed);
    return result;
}

} // namespace cxxime
