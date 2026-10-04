// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/installer_server_process.h>

#include <cstdio>
#include <cwchar>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include <cxxime/ipc_client.h>
#include <cxxime/pipe_names.h>

namespace cxxime {
namespace installer {
namespace {

bool process_gone_error(DWORD error) {
    return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND ||
           error == ERROR_INVALID_HANDLE;
}

bool current_session_id(DWORD* session_id) {
    return session_id != nullptr &&
           ProcessIdToSessionId(GetCurrentProcessId(), session_id) != FALSE;
}

bool query_image_path(HANDLE process, std::wstring* path) {
    if (!process || !path) {
        return false;
    }
    std::vector<wchar_t> buffer(512);
    for (;;) {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length) != FALSE) {
            path->assign(buffer.data(), length);
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool normalize_path(const std::wstring& input, std::wstring* normalized) {
    if (!normalized || input.empty()) {
        return false;
    }
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()),
                                              buffer.data(), nullptr);
        if (length == 0) {
            return false;
        }
        if (length < buffer.size()) {
            normalized->assign(buffer.data(), length);
            return true;
        }
        if (length >= 32768) {
            return false;
        }
        buffer.resize(length + 1);
    }
}

int find_server_process(const std::wstring& expected_path, DWORD* process_id) {
    if (expected_path.empty() || !process_id) {
        return 2;
    }
    std::wstring normalized_expected_path;
    if (!normalize_path(expected_path, &normalized_expected_path)) {
        return 2;
    }
    DWORD session_id = 0;
    if (!current_session_id(&session_id)) {
        return 2;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 2;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = Process32FirstW(snapshot, &entry) != FALSE;
    if (!found) {
        CloseHandle(snapshot);
        return 2;
    }
    while (found) {
        DWORD process_session = 0;
        if (_wcsicmp(entry.szExeFile, L"zhiyi-server.exe") == 0) {
            if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session)) {
                if (process_gone_error(GetLastError())) {
                    found = Process32NextW(snapshot, &entry) != FALSE;
                    continue;
                }
                CloseHandle(snapshot);
                return 2;
            }
        }
        if (_wcsicmp(entry.szExeFile, L"zhiyi-server.exe") == 0 &&
            process_session == session_id) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process) {
                if (process_gone_error(GetLastError())) {
                    found = Process32NextW(snapshot, &entry) != FALSE;
                    continue;
                }
                CloseHandle(snapshot);
                return 2;
            }
            std::wstring image_path;
            const bool queried = query_image_path(process, &image_path);
            const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
            CloseHandle(process);
            if (!queried) {
                if (process_gone_error(query_error)) {
                    found = Process32NextW(snapshot, &entry) != FALSE;
                    continue;
                }
                CloseHandle(snapshot);
                return 2;
            }
            std::wstring normalized_image_path;
            if (!normalize_path(image_path, &normalized_image_path)) {
                CloseHandle(snapshot);
                return 2;
            }
            if (_wcsicmp(normalized_image_path.c_str(), normalized_expected_path.c_str()) == 0) {
                *process_id = entry.th32ProcessID;
                CloseHandle(snapshot);
                return 0;
            }
        }
        found = Process32NextW(snapshot, &entry) != FALSE;
    }
    const DWORD enumeration_error = GetLastError();
    CloseHandle(snapshot);
    return enumeration_error == ERROR_NO_MORE_FILES ? 1 : 2;
}

struct StopWindowContext {
    DWORD process_id = 0;
    bool posted = false;
};

BOOL CALLBACK request_server_close(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<StopWindowContext*>(parameter);
    DWORD process_id = 0;
    if (!context || !GetWindowThreadProcessId(window, &process_id) ||
        process_id != context->process_id || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if (PostMessageW(window, WM_CLOSE, 0, 0) != FALSE) {
        context->posted = true;
    }
    return TRUE;
}

int query_server_process_id(const std::wstring& path, std::uint32_t* process_id) {
    if (!process_id) {
        return 2;
    }
    DWORD native_process_id = 0;
    const int status = find_server_process(path, &native_process_id);
    *process_id = status == 0 ? native_process_id : 0;
    return status;
}

bool query_process_user_name(std::uint32_t process_id, std::wstring* username) {
    if (process_id == 0 || !username) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return false;
    }
    HANDLE token = nullptr;
    const bool token_opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    CloseHandle(process);
    if (!token_opened) {
        return false;
    }

    DWORD token_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
    std::vector<BYTE> token_buffer(token_size);
    if (token_size == 0 ||
        !GetTokenInformation(token, TokenUser, token_buffer.data(), token_size, &token_size)) {
        CloseHandle(token);
        return false;
    }
    auto* token_user = reinterpret_cast<TOKEN_USER*>(token_buffer.data());
    DWORD name_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountSidW(nullptr, token_user->User.Sid, nullptr, &name_size, nullptr, &domain_size,
                      &use);
    std::vector<wchar_t> name(name_size);
    std::vector<wchar_t> domain(domain_size);
    const bool resolved =
        name_size != 0 && LookupAccountSidW(nullptr, token_user->User.Sid, name.data(), &name_size,
                                            domain.data(), &domain_size, &use) != FALSE;
    CloseHandle(token);
    if (!resolved) {
        return false;
    }
    username->assign(name.data());
    return !username->empty();
}

} // namespace

int server_running(const std::wstring& path) {
    DWORD process_id = 0;
    return find_server_process(path, &process_id);
}

int server_ready(const std::wstring& path) {
    std::uint32_t process_id = 0;
    const int running = query_server_process_id(path, &process_id);
    if (running != 0) {
        return running;
    }
    std::wstring username;
    if (!query_process_user_name(process_id, &username)) {
        return 1;
    }
    IpcClient client;
    IPCResponse response = {};
    const std::wstring pipe_name = make_user_pipe_name(IPC_PIPE_BASE_NAME, username);
    return client.connect(pipe_name, 300) && client.ping(&response) &&
                   response.server_process_id == process_id
               ? 0
               : 1;
}

int print_server_pid(const std::wstring& path) {
    DWORD process_id = 0;
    const int status = find_server_process(path, &process_id);
    if (status != 0) {
        return status == 1 ? 1 : 2;
    }
    std::wprintf(L"%lu\n", static_cast<unsigned long>(process_id));
    return 0;
}

int stop_server(const std::wstring& path, bool force) {
    DWORD process_id = 0;
    const int status = find_server_process(path, &process_id);
    if (status != 0) {
        return status == 1 ? 0 : 1;
    }
    if (!force) {
        StopWindowContext context = {process_id, false};
        EnumWindows(request_server_close, reinterpret_cast<LPARAM>(&context));
        if (context.posted) {
            return 0;
        }
        // The server may have exited between the process snapshot and EnumWindows. Treat that
        // race as a successful stop, but preserve real process-query failures.
        DWORD remaining_process_id = 0;
        const int remaining_status = find_server_process(path, &remaining_process_id);
        return remaining_status == 1 ? 0 : 1;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id);
    if (!process) {
        DWORD remaining_process_id = 0;
        const int remaining_status = find_server_process(path, &remaining_process_id);
        return remaining_status == 1 ? 0 : 1;
    }
    const BOOL terminated = TerminateProcess(process, 0) != FALSE;
    bool already_exited = !terminated && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    if (!terminated && !already_exited) {
        DWORD remaining_process_id = 0;
        already_exited = find_server_process(path, &remaining_process_id) == 1;
    }
    return terminated || already_exited ? 0 : 1;
}

int start_server(const std::wstring& path) {
    DWORD session_id = 0;
    if (!current_session_id(&session_id)) {
        return 1;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 1;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    HANDLE token = nullptr;
    bool found_shell = Process32FirstW(snapshot, &entry) != FALSE;
    while (found_shell) {
        DWORD process_session = 0;
        if (_wcsicmp(entry.szExeFile, L"explorer.exe") == 0 &&
            ProcessIdToSessionId(entry.th32ProcessID, &process_session) &&
            process_session == session_id) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (process) {
                OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                                 &token);
                CloseHandle(process);
            }
            if (token) {
                break;
            }
        }
        found_shell = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    if (!token) {
        return 1;
    }

    HANDLE primary_token = nullptr;
    const BOOL duplicated = DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                                             SecurityImpersonation, TokenPrimary,
                                             &primary_token);
    CloseHandle(token);
    if (!duplicated) {
        return 1;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION process = {};
    // The command-line buffer must be writable, and the executable path must be quoted because
    // the default install location contains a space (for example, "Program Files").
    std::wstring command_line = L"\"" + path + L"\"";
    BOOL created = CreateProcessAsUserW(primary_token, path.c_str(), command_line.data(), nullptr,
                                        nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                                        nullptr, &startup, &process);
    if (!created) {
        command_line = L"\"" + path + L"\"";
        created = CreateProcessWithTokenW(primary_token, LOGON_WITH_PROFILE, path.c_str(),
                                          command_line.data(), CREATE_UNICODE_ENVIRONMENT, nullptr,
                                          nullptr, &startup, &process);
    }
    CloseHandle(primary_token);
    if (!created) {
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

} // namespace installer
} // namespace cxxime
