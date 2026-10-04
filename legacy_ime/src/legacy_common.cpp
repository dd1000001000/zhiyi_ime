// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "legacy_common.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>

namespace cxxime_legacy {
namespace {

constexpr wchar_t kInstallRegistryKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZhiyiIME";
constexpr wchar_t kServerExeName[] = L"zhiyi-server.exe";
constexpr wchar_t kSettingsExeName[] = L"zhiyi-settings.exe";

std::wstring query_install_dir_with_view(REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallRegistryKey, 0, KEY_READ | view, &key) !=
        ERROR_SUCCESS) {
        return {};
    }

    WCHAR value[MAX_PATH] = {};
    DWORD cb = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                                         reinterpret_cast<BYTE*>(value), &cb);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ || value[0] == L'\0')
        return {};

    std::wstring path(value);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::wstring query_install_dir() {
#ifdef _WIN64
    std::wstring dir = query_install_dir_with_view(KEY_WOW64_64KEY);
#else
    std::wstring dir = query_install_dir_with_view(KEY_WOW64_32KEY);
#endif
    if (!dir.empty())
        return dir;
    return query_install_dir_with_view(0);
}

} // namespace

std::wstring utf8_to_wide(const char* text) {
    if (!text || !text[0])
        return {};

    const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (required <= 1)
        return {};

    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), required);
    return result;
}

std::wstring truncate_text(std::wstring text) {
    if (text.size() > kMaxCompositionChars)
        text.resize(kMaxCompositionChars);
    return text;
}

bool launch_server() {
    const std::wstring dir = query_install_dir();
    if (dir.empty())
        return false;

    const std::wstring exe = dir + L"\\" + kServerExeName;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    std::wstring command_line = L"\"" + exe + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL ok = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process);
    if (!ok)
        return false;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool launch_settings(HWND parent) {
    const std::wstring dir = query_install_dir();
    if (dir.empty())
        return false;

    const std::wstring exe = dir + L"\\" + kSettingsExeName;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    HINSTANCE result = ShellExecuteW(parent, L"open", exe.c_str(), nullptr, dir.c_str(),
                                     SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool resize_imcc(HIMCC& handle, DWORD bytes) {
    if (handle) {
        HIMCC resized = ImmReSizeIMCC(handle, bytes);
        if (!resized)
            return false;
        handle = resized;
        return true;
    }

    handle = ImmCreateIMCC(bytes);
    return handle != nullptr;
}

void fill_composition_string(COMPOSITIONSTRING& cs,
                             const std::wstring& comp,
                             const std::wstring& result) {
    cs.dwSize = sizeof(CompositionBuffer);
    cs.dwCursorPos = static_cast<DWORD>(comp.size());
    cs.dwDeltaStart = 0;

    if (!comp.empty()) {
        cs.dwCompReadAttrLen = 0;
        cs.dwCompReadClauseLen = 0;
        cs.dwCompReadStrLen = 0;
        cs.dwCompAttrLen = static_cast<DWORD>(comp.size());
        cs.dwCompAttrOffset = offsetof(CompositionBuffer, comp_attr);
        cs.dwCompClauseLen = sizeof(DWORD) * 2;
        cs.dwCompClauseOffset = offsetof(CompositionBuffer, comp_clause);
        cs.dwCompStrLen = static_cast<DWORD>(comp.size());
        cs.dwCompStrOffset = offsetof(CompositionBuffer, comp_str);
    }

    if (!result.empty()) {
        cs.dwResultReadClauseLen = 0;
        cs.dwResultReadStrLen = 0;
        cs.dwResultClauseLen = sizeof(DWORD) * 2;
        cs.dwResultClauseOffset = offsetof(CompositionBuffer, result_clause);
        cs.dwResultStrLen = static_cast<DWORD>(result.size());
        cs.dwResultStrOffset = offsetof(CompositionBuffer, result_str);
    }
}

uint32_t modifiers_from_key_state(const BYTE* key_state) {
    if (!key_state)
        return 0;

    uint32_t modifiers = 0;
    if (key_state[VK_SHIFT] & 0x80)
        modifiers |= 0x01;
    if (key_state[VK_CONTROL] & 0x80)
        modifiers |= 0x02;
    if (key_state[VK_MENU] & 0x80)
        modifiers |= 0x04;
    if (key_state[VK_CAPITAL] & 0x01)
        modifiers |= 0x08;
    return modifiers;
}

bool is_status_key(UINT key_code) {
    return key_code == VK_SHIFT || key_code == VK_LSHIFT || key_code == VK_RSHIFT ||
           key_code == VK_CAPITAL;
}

bool is_key_up_from_key_data(LPARAM key_data) {
    return (static_cast<DWORD>(key_data) & 0x80000000UL) != 0;
}

bool is_ime_ui_message(UINT msg) {
    switch (msg) {
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_NOTIFY:
    case WM_IME_SETCONTEXT:
    case WM_IME_CONTROL:
    case WM_IME_COMPOSITIONFULL:
    case WM_IME_SELECT:
    case WM_IME_CHAR:
        return true;
    default:
        return false;
    }
}

bool should_eat_response(const cxxime::IPCResponse& response,
                         UINT key_code,
                         bool was_composing) {
    if (response.status != cxxime::IPCStatus::OK)
        return false;
    return response.key_handled || response.commit_text[0] || response.preedit[0] ||
        response.composing || response.candidate_count > 0 || was_composing ||
        is_status_key(key_code);
}

DWORD align4(DWORD value) {
    return (value + 3U) & ~3U;
}

} // namespace cxxime_legacy
