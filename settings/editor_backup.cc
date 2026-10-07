// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Backup and restore on the Dictionary page (docs/settings-guide.md): the export holds all
// settings that move between computers and all user data (server/src/user_backup_service.cc);
// an import applies all of it or, when a file is missing or damaged, nothing. The server does
// the work over the control pipe, so it is started first when it is not running.

#include "editor_app.h"

#include <cwchar>
#include <fstream>
#include <string>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/glossary.h>
#include <cxxime/pipe_names.h>
#include <cxxime/server_launcher.h>
#include <cxxime/user_backup.h>
#include <cxxime/user_backup_control.h>

#include "editor_app_internal.h"
#include "glossary_packs.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kBackupTimeoutMs = 60000;  // a large user lexicon takes a while to merge
constexpr int kServerStartWaitMs = 15000;
constexpr wchar_t kBackupExtension[] = L".zhiyi-backup";

// The control pipe exists once the server has loaded and listens.
bool server_listening() {
    const std::wstring pipe = make_user_pipe_name(CONTROL_PIPE_BASE_NAME);
    return WaitNamedPipeW(pipe.c_str(), 1) || GetLastError() == ERROR_SEM_TIMEOUT;
}

// %USERPROFILE%\zhiyi\backup-state.json: the folder the last export went to.
std::wstring backup_state_path() {
    return utf8_to_wstr(user_data_path("backup-state.json"));
}

std::wstring last_backup_folder() {
    std::ifstream file(backup_state_path());
    const nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (state.is_object() && state.contains("folder") && state["folder"].is_string()) {
        std::wstring folder = utf8_to_wstr(state["folder"].get<std::string>());
        const DWORD attributes = GetFileAttributesW(folder.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return folder;
        }
    }
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        folder = documents;
    }
    CoTaskMemFree(documents);
    return folder;
}

void remember_backup_folder(const std::wstring& folder) {
    nlohmann::json state = {{"folder", wstr_to_utf8(folder)}};
    std::ofstream file(backup_state_path(), std::ios::binary | std::ios::trunc);
    file << state.dump(2) << "\n";
}

std::wstring folder_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The Windows file dialog: a folder to export to, or a backup file to import.
std::wstring pick_path(HWND owner, bool folder, const std::wstring& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                       (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dialog->SetTitle(tr(folder ? "backup.pick_folder" : "backup.pick_file"));
    if (!folder) {
        const std::wstring backups = tr("backup.file_type");
        const COMDLG_FILTERSPEC filters[] = {{backups.c_str(), L"*.zhiyi-backup"},
                                             {tr("backup.all_files"), L"*.*"}};
        dialog->SetFileTypes(2, filters);
    }
    IShellItem* start_item = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr,
                                                                IID_PPV_ARGS(&start_item)))) {
        dialog->SetFolder(start_item);
        start_item->Release();
    }
    std::wstring path;
    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            path = name;
            CoTaskMemFree(name);
        }
        result->Release();
    }
    dialog->Release();
    return path;
}

// 知意输入法备份-20261007-1430.zhiyi-backup, with " (2)" and so on when it exists.
std::wstring new_backup_path(const std::wstring& folder) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stamp[32] = {};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute);
    const std::wstring base = folder + L"\\" + tr("backup.file_prefix") + stamp;
    std::wstring path = base + kBackupExtension;
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        path = base + L" (" + std::to_wstring(i) + L")" + kBackupExtension;
    }
    return path;
}

// "2026-10-07T06:30:00Z" in local time: "2026-10-07 14:30".
std::wstring local_time_text(const std::string& utc) {
    SYSTEMTIME time = {};
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf_s(utc.c_str(), "%u-%u-%uT%u:%u:%uZ", &year, &month, &day, &hour, &minute,
                 &second) != 6) {
        return utf8_to_wstr(utc);
    }
    time.wYear = static_cast<WORD>(year);
    time.wMonth = static_cast<WORD>(month);
    time.wDay = static_cast<WORD>(day);
    time.wHour = static_cast<WORD>(hour);
    time.wMinute = static_cast<WORD>(minute);
    time.wSecond = static_cast<WORD>(second);
    SYSTEMTIME local = time;
    SystemTimeToTzSpecificLocalTime(nullptr, &time, &local);
    wchar_t text[32] = {};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute);
    return text;
}

std::wstring size_text(std::uint64_t bytes) {
    wchar_t text[32] = {};
    if (bytes < 1024 * 1024) {
        swprintf_s(text, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    } else {
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return text;
}

std::wstring replace_all(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const std::size_t length = std::wcslen(key);
    for (std::size_t at = text.find(key); at != std::wstring::npos;
         at = text.find(key, at + value.size())) {
        text.replace(at, length, value);
    }
    return text;
}

class WaitCursor {
public:
    WaitCursor() : previous_(SetCursor(LoadCursorW(nullptr, IDC_WAIT))) {}
    ~WaitCursor() { SetCursor(previous_); }

private:
    HCURSOR previous_;
};

} // namespace

bool EditorApp::ensure_server_running() {
    if (server_listening()) {
        return true;
    }
    start_server_on_demand();
    WaitCursor wait;
    for (int waited = 0; waited < kServerStartWaitMs; waited += 200) {
        Sleep(200);
        if (server_listening()) {
            return true;
        }
    }
    MessageBoxW(hwnd_, tr("error.server_unavailable"), tr("window.title"), MB_OK | MB_ICONERROR);
    return false;
}

void EditorApp::export_user_backup() {
    // What the backup holds is what is saved: offer to save the page edits first.
    read_controls(false);
    if (config_.to_user_json() != loaded_config_.to_user_json()) {
        const int answer = MessageBoxW(hwnd_, tr("backup.apply_first"), tr("window.title"),
                                       MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL || (answer == IDYES && !save_config())) {
            return;
        }
    }
    const std::wstring folder = pick_path(hwnd_, true, last_backup_folder());
    if (folder.empty()) {
        return;
    }
    remember_backup_folder(folder);
    if (!ensure_server_running()) {
        return;
    }
    const std::wstring path = new_backup_path(folder);
    UserBackupControlResult result;
    bool exported = false;
    {
        WaitCursor wait;
        exported = UserBackupControlClient(kBackupTimeoutMs)
                       .export_backup(wstr_to_utf8(path), kPortableUserBackupComponents,
                                      &result) &&
                   result.succeeded;
    }
    if (!exported) {
        MessageBoxW(hwnd_, tr("backup.export_failed"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring message = replace_all(tr("backup.exported"), L"{path}", path);
    if (MessageBoxW(hwnd_, message.c_str(), tr("window.title"), MB_YESNO | MB_ICONINFORMATION) ==
        IDYES) {
        const std::wstring arguments = L"/select,\"" + path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void EditorApp::import_user_backup() {
    const std::wstring path = pick_path(hwnd_, false, last_backup_folder());
    if (path.empty() || !ensure_server_running()) {
        return;
    }
    remember_backup_folder(folder_of(path));
    const std::string utf8_path = wstr_to_utf8(path);
    UserBackupControlClient client(kBackupTimeoutMs);
    UserBackupControlResult summary;
    if (!client.inspect(utf8_path, &summary) || !summary.succeeded) {
        MessageBoxW(hwnd_, tr("backup.import_invalid"), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring confirm = tr("backup.import_confirm");
    confirm = replace_all(confirm, L"{version}", utf8_to_wstr(summary.summary.app_version));
    confirm = replace_all(confirm, L"{time}", local_time_text(summary.summary.created_at_utc));
    confirm = replace_all(confirm, L"{size}", size_text(summary.summary.total_size));
    if (MessageBoxW(hwnd_, confirm.c_str(), tr("window.title"), MB_OKCANCEL | MB_ICONQUESTION) !=
        IDOK) {
        return;
    }

    UserBackupControlResult result;
    bool sent = false;
    {
        WaitCursor wait;
        sent = client.import_backup(utf8_path, kPortableUserBackupComponents, &result);
    }
    if (!sent || !result.succeeded) {
        const char* reason = !sent ? "backup.import_failed"
                             : result.error_code == ERROR_FILE_NOT_FOUND ? "backup.import_missing"
                             : result.error_code == ERROR_HOTKEY_ALREADY_REGISTERED
                                 ? "backup.import_hotkey"
                             : result.error_code == ERROR_INVALID_DATA ? "backup.import_invalid"
                                                                       : "backup.import_failed";
        MessageBoxW(hwnd_, tr(reason), tr("window.title"), MB_OK | MB_ICONERROR);
        return;
    }

    // Show the imported settings (in their interface language).
    if (load_config()) {
        ui_language_ = resolve_ui_language(config_.ui_language);
        load_ui_strings(ui_language_);
        rebuild_ui();
    }
    MessageBoxW(hwnd_, tr("backup.imported"), tr("window.title"), MB_OK | MB_ICONINFORMATION);

    // Translations need their language packs on this computer.
    std::wstring missing;
    for (const auto& [source, target] : {std::make_pair(std::string("zh"), config_.chinese_gloss_target),
                                         std::make_pair(std::string("en"), config_.english_gloss_target)}) {
        if (!target.empty() && !installed_pack(source, target).installed) {
            if (!missing.empty()) missing += L"、";
            missing += tr(("lang." + source).c_str());
            missing += L" → ";
            missing += tr(("lang." + target).c_str());
        }
    }
    if (!missing.empty()) {
        const std::wstring message = replace_all(tr("backup.packs_missing"), L"{packs}", missing);
        if (MessageBoxW(hwnd_, message.c_str(), tr("window.title"),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            show_panel(kLearningPanel);
        }
    }
}

void EditorApp::open_backup_folder() {
    const std::wstring folder = utf8_to_wstr(user_data_path("backups"));
    CreateDirectoryW(folder.c_str(), nullptr);
    ShellExecuteW(hwnd_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace settings
} // namespace cxxime
