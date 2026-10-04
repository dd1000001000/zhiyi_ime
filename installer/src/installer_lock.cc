// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/installer_lock.h>

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <restartmanager.h>

namespace cxxime {
namespace installer {
namespace {

class RestartManagerSession {
public:
    RestartManagerSession() = default;

    ~RestartManagerSession() {
        if (active_) {
            RmEndSession(handle_);
        }
    }

    RestartManagerSession(const RestartManagerSession&) = delete;
    RestartManagerSession& operator=(const RestartManagerSession&) = delete;

    DWORD start() {
        WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
        const DWORD result = RmStartSession(&handle_, 0, key);
        active_ = result == ERROR_SUCCESS;
        return result;
    }

    DWORD handle() const { return handle_; }

private:
    DWORD handle_ = 0;
    bool active_ = false;
};

std::wstring absolute_existing_file(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }

    const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (length == 0) {
        return {};
    }

    std::wstring absolute(static_cast<std::size_t>(length), L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), length, &absolute[0], nullptr);
    if (written == 0 || written >= length) {
        return {};
    }
    absolute.resize(static_cast<std::size_t>(written));
    return absolute;
}

void append_application(const RM_PROCESS_INFO& process, LockQueryResult* result) {
    const std::uint32_t process_id = process.Process.dwProcessId;
    const auto duplicate = std::find_if(result->applications.begin(), result->applications.end(),
                                        [process_id](const LockingApplication& application) {
                                            return application.process_id == process_id;
                                        });
    if (duplicate != result->applications.end()) {
        return;
    }

    LockingApplication application;
    application.process_id = process_id;
    application.name = process.strAppName;
    result->applications.push_back(std::move(application));
}

std::wstring bounded_application_name(const std::wstring& name) {
    constexpr std::size_t kMaximumNameLength = 80;
    if (name.size() <= kMaximumNameLength) {
        return name;
    }
    return name.substr(0, kMaximumNameLength - 3) + L"...";
}

} // namespace

LockQueryResult query_file_locks(const std::vector<std::wstring>& paths) {
    LockQueryResult result;
    std::vector<std::wstring> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        std::wstring absolute = absolute_existing_file(path);
        if (!absolute.empty()) {
            files.push_back(std::move(absolute));
        }
    }
    if (files.empty()) {
        return result;
    }

    RestartManagerSession session;
    DWORD status = session.start();
    if (status != ERROR_SUCCESS) {
        result.status = LockQueryStatus::kFailed;
        result.error_code = status;
        return result;
    }

    std::vector<LPCWSTR> file_names;
    file_names.reserve(files.size());
    for (const auto& file : files) {
        file_names.push_back(file.c_str());
    }

    status = RmRegisterResources(session.handle(), static_cast<UINT>(file_names.size()),
                                 file_names.data(), 0, nullptr, 0, nullptr);
    if (status != ERROR_SUCCESS) {
        result.status = LockQueryStatus::kFailed;
        result.error_code = status;
        return result;
    }

    UINT needed = 0;
    UINT count = 0;
    DWORD reboot_reasons = RmRebootReasonNone;
    status = RmGetList(session.handle(), &needed, &count, nullptr, &reboot_reasons);
    for (int attempt = 0; status == ERROR_MORE_DATA && attempt < 4; ++attempt) {
        std::vector<RM_PROCESS_INFO> processes(needed);
        count = needed;
        status = RmGetList(session.handle(), &needed, &count, processes.data(), &reboot_reasons);
        if (status == ERROR_SUCCESS) {
            for (UINT index = 0; index < count; ++index) {
                append_application(processes[index], &result);
            }
        }
    }

    if (status != ERROR_SUCCESS) {
        result.status = LockQueryStatus::kFailed;
        result.error_code = status;
        return result;
    }

    result.reboot_reasons = reboot_reasons;
    if (reboot_reasons != RmRebootReasonNone) {
        result.status = LockQueryStatus::kRebootRequired;
    }
    return result;
}

std::wstring format_lock_report(const LockQueryResult& result, std::size_t max_applications) {
    if (result.status == LockQueryStatus::kFailed) {
        return L"Windows 重启管理器无法检查文件占用情况，错误代码：" +
               std::to_wstring(result.error_code) + L"。";
    }

    std::wstring report;
    if (result.status == LockQueryStatus::kRebootRequired) {
        report = L"Windows 要求重新启动后才能更新知意输入法。";
    } else if (!result.applications.empty()) {
        report = L"以下应用程序正在使用知意输入法：";
    }

    const std::size_t count = std::min(result.applications.size(), max_applications);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& application = result.applications[index];
        const std::wstring name = application.name.empty()
            ? L"未知应用程序"
            : bounded_application_name(application.name);
        report += L"\r\n- " + name + L" (PID " + std::to_wstring(application.process_id) + L")";
    }
    if (result.applications.size() > count) {
        report += L"\r\n- ……以及另外 " + std::to_wstring(result.applications.size() - count) +
                  L" 个应用程序";
    }
    return report;
}

} // namespace installer
} // namespace cxxime
