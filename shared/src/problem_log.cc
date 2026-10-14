// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/problem_log.h>

#include <windows.h>

#include <atomic>
#include <cstdio>

#include <cxxime/diagnostic_log_path.h>

namespace cxxime {
namespace {

std::atomic<bool> g_disabled{false};

std::string process_name() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
    }
    char utf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

}  // namespace

void log_problem_to(const std::wstring& directory, const char* area, const std::string& message) {
    if (directory.empty()) return;
    const std::wstring path = directory + L"\\" + kProblemLogName;

    // One writer at a time across the server, settings and the input hosts.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ZhiyuIME.ProblemLog");
    if (mutex) WaitForSingleObject(mutex, 2000);

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) &&
        ((static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow) >=
            kProblemLogMaxBytes) {
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char stamp[32] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string line = std::string(stamp) + " " + process_name() + " [" + area + "] " + message;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    line += "\r\n";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(file);
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
}

void log_problem(const char* area, const std::string& message) {
    if (g_disabled.load()) return;
    log_problem_to(diagnostic_log_directory(), area, message);
}

void disable_problem_log() { g_disabled.store(true); }

std::string windows_error_text(unsigned long error_code) {
    wchar_t* buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, error_code,
                                  MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                  reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0) {  // no English resources on this Windows: its own language
        length = FormatMessageW(flags, nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&buffer),
                                0, nullptr);
    }
    std::string text;
    if (length > 0 && buffer) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr,
                                             0, nullptr, nullptr);
        text.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), size,
                            nullptr, nullptr);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = "Error";
    return text + " (" + std::to_string(error_code) + ")";
}

}  // namespace cxxime
