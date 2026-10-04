// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <shellapi.h>
#include <cxxime/diagnostic_log_path.h>

#include "diagnostic_log_cleanup.h"
#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kDiagnosticsLoggingId = 6001;
constexpr int kExportDiagnosticsId = 6002;
constexpr int kOpenDiagnosticsDirectoryId = 6003;
constexpr int kCleanupDiagnosticsId = 6004;

std::wstring module_directory() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        return {};
    }
    std::wstring directory(path);
    size_t separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        directory.resize(separator);
    }
    return directory;
}

std::wstring find_collect_diagnostics_script() {
    std::wstring directory = module_directory();
    if (directory.empty()) {
        return {};
    }
    std::wstring script = directory + L"\\collect_diagnostics.ps1";
    DWORD attributes = GetFileAttributesW(script.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return script;
    }
    return {};
}

std::wstring format_deleted_size(std::uint64_t bytes) {
    std::wostringstream text;
    if (bytes < 1024ULL * 1024ULL) {
        text << (bytes + 1023ULL) / 1024ULL << L" KiB";
    } else {
        text.setf(std::ios::fixed);
        text.precision(1);
        text << static_cast<double>(bytes) / (1024.0 * 1024.0) << L" MiB";
    }
    return text.str();
}

} // namespace

void EditorApp::create_diagnostics_panel(HWND panel) {
    const int top = kPanelPadTop;
    SetWindowSubclass(panel, PanelForwardProc, 6000, reinterpret_cast<DWORD_PTR>(hwnd_));

    hDiagnosticsLogging_ =
        make_check(kDiagnosticsLoggingId, L"启用诊断日志", kPanelPadLeft, top, S(180), panel);
    const int content_x = kPanelPadLeft + S(22);

    HWND privacy_notice = CreateWindowExW(
        0, L"STATIC", L"诊断日志可能包含输入编码。", WS_CHILD | WS_VISIBLE | SS_LEFT, content_x,
        top + kRowH, S(260), kCtrlH, panel, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(privacy_notice, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    const int directory_y = top + kRowH * 3;
    HWND open_directory_button = CreateWindowExW(
        0, L"BUTTON", L"打开常规日志目录", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        kPanelPadLeft, directory_y, S(150), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenDiagnosticsDirectoryId)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(open_directory_button, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    const int packaged_app_y = directory_y + kRowH;
    RECT panel_rect = {};
    GetClientRect(panel, &panel_rect);
    const int packaged_app_width = panel_rect.right - content_x - S(8);
    HWND packaged_app_path =
        CreateWindowExW(0, L"STATIC", L"PackagedApp 日志目录: LocalState\\zhiyi\\logs",
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, content_x,
                        packaged_app_y, packaged_app_width, kCtrlH, panel, nullptr,
                        GetModuleHandle(nullptr), nullptr);
    SendMessageW(packaged_app_path, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    HWND export_button = CreateWindowExW(
        0, L"BUTTON", L"导出诊断包", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        kPanelPadLeft, top + kRowH * 6, S(150), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportDiagnosticsId)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(export_button, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    hDiagnosticsCleanup_ = CreateWindowExW(
        0, L"BUTTON", L"清理历史日志", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        kPanelPadLeft + S(160), top + kRowH * 6, S(150), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCleanupDiagnosticsId)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(hDiagnosticsCleanup_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    HWND cleanup_notice = CreateWindowExW(
        0, L"STATIC",
        L"清理常规和 PackagedApp 历史日志；正在使用的文件会被跳过，启用诊断时仍会生成新日志。",
        WS_CHILD | WS_VISIBLE | SS_LEFT, kPanelPadLeft, top + kRowH * 7,
        panel_rect.right - kPanelPadLeft - S(8), kCtrlH * 2, panel, nullptr,
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(cleanup_notice, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
}

bool EditorApp::handle_diagnostics_command(int control_id, int notification) {
    if (control_id == kExportDiagnosticsId && notification == BN_CLICKED) {
        export_diagnostics();
        return true;
    }
    if (control_id == kOpenDiagnosticsDirectoryId && notification == BN_CLICKED) {
        open_diagnostics_log_directory();
        return true;
    }
    if (control_id == kCleanupDiagnosticsId && notification == BN_CLICKED) {
        cleanup_diagnostics();
        return true;
    }
    return false;
}

void EditorApp::load_diagnostics_controls() {
    set_check(hDiagnosticsLogging_,
              config_.diagnostics.trace_mode != cxxime::DiagnosticTraceMode::kOff);
}

void EditorApp::read_diagnostics_controls() {
    if (!get_check(hDiagnosticsLogging_)) {
        config_.diagnostics.trace_mode = cxxime::DiagnosticTraceMode::kOff;
    } else if (config_.diagnostics.trace_mode == cxxime::DiagnosticTraceMode::kOff) {
        config_.diagnostics.trace_mode = cxxime::DiagnosticTraceMode::kNormal;
    }
}

void EditorApp::open_diagnostics_log_directory() {
    const std::wstring directory = cxxime::diagnostic_log_directory();
    if (directory.empty()) {
        MessageBoxW(hwnd_, L"无法确定诊断日志目录。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }

    HINSTANCE result =
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(hwnd_, L"无法打开诊断日志目录。", L"知意输入法", MB_OK | MB_ICONERROR);
    }
}

void EditorApp::export_diagnostics() {
    std::wstring script = find_collect_diagnostics_script();
    if (script.empty()) {
        MessageBoxW(hwnd_, L"未找到 collect_diagnostics.ps1。请确认当前版本已完整安装。", L"知意输入法",
                    MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring directory = script;
    size_t separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        directory.resize(separator);
    }

    std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    SHELLEXECUTEINFOW execute_info = {};
    execute_info.cbSize = sizeof(execute_info);
    execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute_info.hwnd = hwnd_;
    execute_info.lpVerb = L"open";
    execute_info.lpFile = L"powershell.exe";
    execute_info.lpParameters = parameters.c_str();
    execute_info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    execute_info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute_info)) {
        MessageBoxW(hwnd_, L"启动诊断导出失败。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(hwnd_, L"已开始导出诊断包，完成后会再次提示结果。", L"知意输入法",
                MB_OK | MB_ICONINFORMATION);

    if (execute_info.hProcess) {
        HWND window = hwnd_;
        HANDLE process = execute_info.hProcess;
        std::thread([window, process]() {
            WaitForSingleObject(process, INFINITE);
            DWORD exit_code = 1;
            GetExitCodeProcess(process, &exit_code);
            CloseHandle(process);
            if (IsWindow(window)) {
                PostMessageW(window, kDiagnosticsCompleteMessage, static_cast<WPARAM>(exit_code),
                             0);
            }
        }).detach();
    }
}

void EditorApp::cleanup_diagnostics() {
    if (!IsWindowEnabled(hDiagnosticsCleanup_)) {
        return;
    }
    if (MessageBoxW(hwnd_, L"将删除当前未被使用的知意输入法历史诊断日志。是否继续？", L"知意输入法",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    EnableWindow(hDiagnosticsCleanup_, FALSE);
    HWND window = hwnd_;
    try {
        std::thread([window]() {
            std::unique_ptr<DiagnosticsCleanupSummary> summary;
            try {
                summary = std::make_unique<DiagnosticsCleanupSummary>(
                    cleanup_current_user_diagnostic_logs());
            } catch (...) {
            }
            if (!IsWindow(window) ||
                !PostMessageW(window, kDiagnosticsCleanupCompleteMessage, 0,
                              reinterpret_cast<LPARAM>(summary.get()))) {
                return;
            }
            summary.release();
        }).detach();
    } catch (...) {
        EnableWindow(hDiagnosticsCleanup_, TRUE);
        MessageBoxW(hwnd_, L"无法启动日志清理任务。", L"知意输入法", MB_OK | MB_ICONERROR);
    }
}

void EditorApp::handle_diagnostics_cleanup_complete(LPARAM completion) {
    std::unique_ptr<DiagnosticsCleanupSummary> summary(
        reinterpret_cast<DiagnosticsCleanupSummary*>(completion));
    EnableWindow(hDiagnosticsCleanup_, TRUE);
    if (!summary) {
        MessageBoxW(hwnd_, L"清理历史日志失败。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }

    std::wostringstream message;
    message << L"已删除 " << summary->deleted_files << L" 个历史日志文件，释放 "
            << format_deleted_size(summary->deleted_bytes) << L"。";
    if (summary->skipped_files > 0 || summary->inaccessible_directories > 0) {
        message << L"\n\n" << summary->skipped_files
                << L" 个文件正在使用或无法删除，另有 "
                << summary->inaccessible_directories << L" 个目录无法访问，已跳过。";
    }
    if (get_check(hDiagnosticsLogging_)) {
        message << L"\n\n诊断日志已启用，运行中的应用仍会继续生成新日志。";
    }
    MessageBoxW(hwnd_, message.str().c_str(), L"知意输入法", MB_OK | MB_ICONINFORMATION);
}

} // namespace settings
} // namespace cxxime
