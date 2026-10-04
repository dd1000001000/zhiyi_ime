// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <thread>

#include <commdlg.h>

#include <cxxime/user_backup.h>
#include <cxxime/user_backup_control.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kBackupExportId = 7001;
constexpr int kBackupSelectId = 7002;
constexpr int kBackupImportId = 7003;
constexpr int kExportComponentBaseId = 7010;
constexpr int kImportComponentBaseId = 7020;

constexpr std::array<UserBackupComponent, 6> kComponents = {
    UserBackupComponent::kSettings,
    UserBackupComponent::kUserLexicon,
    UserBackupComponent::kCandidateOrder,
    UserBackupComponent::kLearning,
    UserBackupComponent::kDisabledSystemLexicon,
    UserBackupComponent::kDeviceSettings,
};

constexpr std::array<const wchar_t*, 6> kComponentLabels = {
    L"设置", L"手工词库", L"候选排序", L"学习记录", L"停用系统词", L"窗口位置和诊断设置",
};

bool has_component(std::uint32_t components, UserBackupComponent component) {
    return (components & user_backup_component_flag(component)) != 0;
}

struct BackupCompletion {
    UserBackupOperation operation = UserBackupOperation::kUnknown;
    UserBackupControlResult result;
    std::string path;
    std::uint32_t components = 0;
    std::shared_ptr<const bool> token;
};

std::wstring backup_file_name() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t name[80] = {};
    swprintf_s(name, L"ZhiyiIME-backup-%04u%02u%02u-%02u%02u.zhiyi-backup", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute);
    return name;
}

std::wstring format_backup_summary(const UserBackupSummary& summary) {
    std::wostringstream text;
    text << L"版本 " << utf8_to_wstr(summary.app_version) << L"  |  "
         << utf8_to_wstr(summary.created_at_utc) << L"  |  " << summary.entry_count
         << L" 个文件  |  ";
    if (summary.total_size < 1024ULL * 1024ULL) {
        text << (summary.total_size + 1023ULL) / 1024ULL << L" KiB";
    } else {
        text.setf(std::ios::fixed);
        text.precision(1);
        text << static_cast<double>(summary.total_size) / (1024.0 * 1024.0) << L" MiB";
    }
    return text.str();
}

std::wstring error_message(std::uint32_t error_code) {
    wchar_t* system_text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&system_text), 0, nullptr);
    std::wstring result = length && system_text ? system_text : L"操作失败";
    if (system_text) {
        LocalFree(system_text);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

} // namespace

void EditorApp::create_backup_panel(HWND panel, int panel_width) {
    const int top = kPanelPadTop;
    const int left = kPanelPadLeft;
    const int second_column = left + S(170);
    SetWindowSubclass(panel, PanelForwardProc, 7000, reinterpret_cast<DWORD_PTR>(hwnd_));

    HWND export_title =
        CreateWindowExW(0, L"STATIC", L"创建备份", WS_CHILD | WS_VISIBLE | SS_LEFT, left, top,
                        S(180), kCtrlH, panel, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(export_title, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    for (std::size_t index = 0; index < kComponents.size(); ++index) {
        const int column = static_cast<int>(index % 2);
        const int row = static_cast<int>(index / 2);
        hBackupExportComponents_[index] =
            make_check(kExportComponentBaseId + static_cast<int>(index), kComponentLabels[index],
                       column == 0 ? left : second_column, top + kRowH * (row + 1),
                       column == 0 ? S(150) : S(220), panel);
        set_check(hBackupExportComponents_[index],
                  kComponents[index] != UserBackupComponent::kDeviceSettings);
    }
    hBackupExport_ = CreateWindowExW(0, L"BUTTON", L"导出备份...",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, left,
                                     top + kRowH * 4, S(130), kCtrlH, panel,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackupExportId)),
                                     GetModuleHandle(nullptr), nullptr);
    SendMessageW(hBackupExport_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    const int import_top = top + kRowH * 5;
    HWND import_title = CreateWindowExW(0, L"STATIC", L"导入备份", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        left, import_top, S(105), kCtrlH, panel, nullptr,
                                        GetModuleHandle(nullptr), nullptr);
    SendMessageW(import_title, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hBackupStatus_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_ENDELLIPSIS,
                                     left + S(110), import_top, panel_width - left - S(120), kCtrlH,
                                     panel, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hBackupStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hBackupSelect_ = CreateWindowExW(0, L"BUTTON", L"选择备份...",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, left,
                                     import_top + kRowH, S(130), kCtrlH, panel,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackupSelectId)),
                                     GetModuleHandle(nullptr), nullptr);
    SendMessageW(hBackupSelect_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hBackupImport_ = CreateWindowExW(0, L"BUTTON", L"导入所选内容",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     left + S(145), import_top + kRowH, S(130), kCtrlH, panel,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackupImportId)),
                                     GetModuleHandle(nullptr), nullptr);
    SendMessageW(hBackupImport_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    EnableWindow(hBackupImport_, FALSE);
    for (std::size_t index = 0; index < kComponents.size(); ++index) {
        const int column = static_cast<int>(index % 2);
        const int row = static_cast<int>(index / 2);
        hBackupImportComponents_[index] =
            make_check(kImportComponentBaseId + static_cast<int>(index), kComponentLabels[index],
                       column == 0 ? left : second_column, import_top + kRowH * (row + 2),
                       column == 0 ? S(150) : S(220), panel);
        EnableWindow(hBackupImportComponents_[index], FALSE);
    }
}

std::uint32_t EditorApp::selected_backup_components(bool importing) const {
    std::uint32_t components = 0;
    const HWND* controls = importing ? hBackupImportComponents_ : hBackupExportComponents_;
    for (std::size_t index = 0; index < kComponents.size(); ++index) {
        if (get_check(controls[index])) {
            components |= user_backup_component_flag(kComponents[index]);
        }
    }
    return components;
}

void EditorApp::set_backup_controls_enabled(bool enabled) {
    backupRunning_ = !enabled;
    EnableWindow(hBackupExport_, enabled);
    EnableWindow(hBackupSelect_, enabled);
    for (HWND control : hBackupExportComponents_) {
        EnableWindow(control, enabled);
    }
    for (std::size_t index = 0; index < kComponents.size(); ++index) {
        const bool available =
            (availableBackupComponents_ & user_backup_component_flag(kComponents[index])) != 0;
        EnableWindow(hBackupImportComponents_[index], enabled && available);
    }
    EnableWindow(hBackupImport_,
                 enabled && !selectedBackupPath_.empty() && selected_backup_components(true) != 0);
    for (int id : {2001, 2002, 2003}) {
        EnableWindow(GetDlgItem(hwnd_, id), enabled);
    }
}

bool EditorApp::handle_backup_command(int control_id, int notification) {
    if (notification != BN_CLICKED || backupRunning_) {
        return false;
    }
    if (control_id == kBackupExportId) {
        export_user_backup();
        return true;
    }
    if (control_id == kBackupSelectId) {
        select_user_backup();
        return true;
    }
    if (control_id == kBackupImportId) {
        import_user_backup();
        return true;
    }
    if (control_id >= kImportComponentBaseId &&
        control_id < kImportComponentBaseId + static_cast<int>(kComponents.size())) {
        EnableWindow(hBackupImport_, selected_backup_components(true) != 0);
        return true;
    }
    return (control_id >= kExportComponentBaseId &&
            control_id < kExportComponentBaseId + static_cast<int>(kComponents.size()));
}

void EditorApp::export_user_backup() {
    const std::uint32_t components = selected_backup_components(false);
    if (components == 0) {
        MessageBoxW(hwnd_, L"请至少选择一项备份内容。", L"知意输入法", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring file = backup_file_name();
    std::array<wchar_t, 32768> buffer = {};
    wcscpy_s(buffer.data(), buffer.size(), file.c_str());
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"知意输入法备份 (*.zhiyi-backup)\0*.zhiyi-backup\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrDefExt = L"zhiyi-backup";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&dialog)) {
        if ((has_component(components, UserBackupComponent::kSettings) ||
             has_component(components, UserBackupComponent::kDeviceSettings)) &&
            !save_config()) {
            return;
        }
        run_user_backup_operation(UserBackupOperation::kExport, wstr_to_utf8(buffer.data()),
                                  components);
    }
}

void EditorApp::select_user_backup() {
    std::array<wchar_t, 32768> buffer = {};
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"知意输入法备份 (*.zhiyi-backup)\0*.zhiyi-backup\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        selectedBackupPath_.clear();
        availableBackupComponents_ = 0;
        ShowWindow(hBackupStatus_, SW_HIDE);
        for (HWND control : hBackupImportComponents_) {
            set_check(control, false);
            EnableWindow(control, FALSE);
        }
        EnableWindow(hBackupImport_, FALSE);
        run_user_backup_operation(UserBackupOperation::kInspect, wstr_to_utf8(buffer.data()), 0);
    }
}

void EditorApp::import_user_backup() {
    const std::uint32_t components = selected_backup_components(true);
    if (selectedBackupPath_.empty() || components == 0) {
        MessageBoxW(hwnd_, L"请至少选择一项导入内容。", L"知意输入法", MB_OK | MB_ICONINFORMATION);
        return;
    }
    run_user_backup_operation(UserBackupOperation::kImport, selectedBackupPath_, components);
}

void EditorApp::run_user_backup_operation(UserBackupOperation operation, const std::string& path,
                                          std::uint32_t components) {
    set_backup_controls_enabled(false);
    if (operation == UserBackupOperation::kInspect) {
        SetWindowTextW(hBackupSelect_, L"正在检查...");
    } else if (operation == UserBackupOperation::kExport) {
        SetWindowTextW(hBackupExport_, L"正在导出...");
    } else {
        SetWindowTextW(hBackupImport_, L"正在导入...");
    }
    const HWND window = hwnd_;
    const auto token = backupToken_;
    std::thread([window, operation, path, components, token]() {
        BackupCompletion completion;
        completion.operation = operation;
        completion.path = path;
        completion.components = components;
        completion.token = token;
        UserBackupControlClient client;
        if (operation == UserBackupOperation::kInspect) {
            client.inspect(path, &completion.result);
        } else if (operation == UserBackupOperation::kExport) {
            client.export_backup(path, components, &completion.result);
        } else {
            client.import_backup(path, components, &completion.result);
        }
        if (IsWindow(window)) {
            SendMessageW(window, kUserBackupCompleteMessage, 0,
                         reinterpret_cast<LPARAM>(&completion));
        }
    }).detach();
}

void EditorApp::handle_backup_complete(LPARAM completion_data) {
    const auto* completion = reinterpret_cast<const BackupCompletion*>(completion_data);
    if (!completion || completion->token != backupToken_) {
        return;
    }
    SetWindowTextW(hBackupExport_, L"导出备份...");
    SetWindowTextW(hBackupSelect_, L"选择备份...");
    SetWindowTextW(hBackupImport_, L"导入所选内容");
    set_backup_controls_enabled(true);
    if (!completion->result.succeeded) {
        std::wstring message = L"操作失败：" + error_message(completion->result.error_code);
        MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    if (completion->operation == UserBackupOperation::kInspect) {
        selectedBackupPath_ = completion->path;
        availableBackupComponents_ = completion->result.summary.components;
        for (std::size_t index = 0; index < kComponents.size(); ++index) {
            const bool available =
                (availableBackupComponents_ & user_backup_component_flag(kComponents[index])) != 0;
            set_check(hBackupImportComponents_[index],
                      available && kComponents[index] != UserBackupComponent::kDeviceSettings);
        }
        set_backup_controls_enabled(true);
        const std::wstring summary = format_backup_summary(completion->result.summary);
        SetWindowTextW(hBackupStatus_, summary.c_str());
        ShowWindow(hBackupStatus_, SW_SHOW);
        return;
    }
    if (completion->operation == UserBackupOperation::kExport) {
        const std::wstring message = L"备份已导出到：\n" + path_for_display(completion->path);
        MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (has_component(completion->components, UserBackupComponent::kSettings) ||
        has_component(completion->components, UserBackupComponent::kDeviceSettings)) {
        initial_panel_ = cxxime::SettingsPanel::kBackup;
        load_config();
    }
    std::wostringstream message;
    message << L"已合并 " << completion->result.imported_count << L" 项";
    if (completion->result.skipped_count != 0) {
        message << L"，跳过 " << completion->result.skipped_count << L" 项";
    }
    message << L"。";
    MessageBoxW(hwnd_, message.str().c_str(), L"知意输入法", MB_OK | MB_ICONINFORMATION);
}

} // namespace settings
} // namespace cxxime
