// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <cstdio>
#include <thread>

#include <commdlg.h>
#include <shellapi.h>

#include <cxxime/data_path.h>
#include <cxxime/lexicon_control.h>
#include <cxxime/user_dict_validation.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

struct LexiconImportCompletion {
    UserDictKind kind = UserDictKind::PINYIN;
    bool succeeded = false;
    std::shared_ptr<const bool> token;
};

bool import_file_size_is_valid(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
        return false;
    }
    ULARGE_INTEGER size = {};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart <= kMaxUserDictImportBytes;
}

std::wstring file_last_write_time_text(const std::string& path) {
    const std::wstring wide_path = path_for_display(path);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(wide_path.c_str(), GetFileExInfoStandard, &data)) {
        return L"未创建";
    }

    FILETIME local_time = {};
    SYSTEMTIME system_time = {};
    if (!FileTimeToLocalFileTime(&data.ftLastWriteTime, &local_time) ||
        !FileTimeToSystemTime(&local_time, &system_time)) {
        return L"未知";
    }

    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", system_time.wYear, system_time.wMonth,
               system_time.wDay, system_time.wHour, system_time.wMinute);
    return buffer;
}

bool copy_file_utf8_path(const std::string& source, const std::string& destination) {
    const std::wstring wide_source = path_for_display(source);
    const std::wstring wide_destination = path_for_display(destination);
    return CopyFileW(wide_source.c_str(), wide_destination.c_str(), FALSE) != FALSE;
}

} // namespace

std::string EditorApp::current_user_dict_path() const {
    const bool wubi = current_user_dict_kind() == UserDictKind::WUBI;
    if (current_lexicon_resource() == LexiconResource::kCandidatePreference) {
        return user_data_path(wubi ? "learning_wubi.tsv" : "learning_pinyin.tsv");
    }
    if (current_lexicon_resource() == LexiconResource::kManualCandidateOrder) {
        return user_data_path(wubi ? "candidate_order_wubi.tsv" : "candidate_order_pinyin.tsv");
    }
    return user_data_path(wubi ? "user_wubi.tsv" : "user_pinyin.tsv");
}

void EditorApp::update_lexicon_status() {
    if (!hLexiconStatus_) {
        return;
    }
    const std::wstring modified = file_last_write_time_text(current_user_dict_path());
    const int shown = hLexiconList_ ? ListView_GetItemCount(hLexiconList_) : 0;
    wchar_t buffer[160] = {};
    swprintf_s(buffer, L"显示 %d 条，用户数据更新于 %s", shown, modified.c_str());
    SetWindowTextW(hLexiconStatus_, buffer);
}

void EditorApp::clear_candidate_preferences() {
    if (current_lexicon_resource() != LexiconResource::kCandidatePreference ||
        lexiconImportRunning_) {
        return;
    }
    const wchar_t* kind = current_user_dict_kind() == UserDictKind::WUBI ? L"五笔" : L"拼音";
    std::wstring message = L"清空全部";
    message += kind;
    message += L"选词偏好？\n\n用户词库不会受到影响。";
    if (MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    if (!client.clear_preferences(current_user_dict_kind(), &result)) {
        MessageBoxW(hwnd_, L"清空选词偏好失败。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    clear_lexicon_entry_form();
    query_lexicon_entries(false);
    SetWindowTextW(hLexiconStatus_, L"选词偏好已清空，正在刷新...");
}

void EditorApp::import_user_dict() {
    if (current_lexicon_resource() != LexiconResource::kUserLexicon || lexiconImportRunning_) {
        return;
    }
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"TSV 用户词库 (*.tsv)\0*.tsv\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }
    if (MessageBoxW(hwnd_, L"导入会覆盖当前用户词库，是否继续？", L"知意输入法",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    if (!import_file_size_is_valid(file)) {
        MessageBoxW(hwnd_, L"导入失败：文件无法读取或超过 64 MiB。", L"知意输入法",
                    MB_OK | MB_ICONERROR);
        return;
    }

    lexiconImportRunning_ = true;
    update_lexicon_entry_actions();
    SetWindowTextW(hLexiconStatus_, L"正在导入用户词库...");
    const HWND window = hwnd_;
    const UserDictKind kind = current_user_dict_kind();
    const std::string source_path = wstr_to_utf8(file);
    const auto token = lexiconImportToken_;
    std::thread([window, kind, source_path, token]() {
        LexiconImportCompletion completion;
        completion.kind = kind;
        completion.token = token;
        LexiconControlClient client;
        LexiconControlResult result;
        completion.succeeded = client.import_entries(kind, source_path, &result);
        SendMessageW(window, kLexiconImportCompleteMessage, 0,
                     reinterpret_cast<LPARAM>(&completion));
    }).detach();
}

void EditorApp::handle_lexicon_import_complete(LPARAM completion_data) {
    const auto* completion = reinterpret_cast<const LexiconImportCompletion*>(completion_data);
    if (!completion || completion->token != lexiconImportToken_) {
        return;
    }
    lexiconImportRunning_ = false;
    update_lexicon_entry_actions();
    if (!completion->succeeded) {
        SetWindowTextW(hLexiconStatus_, L"导入失败");
        MessageBoxW(hwnd_, L"导入失败：知意输入法后台未运行，或文件无效、无法保存。", L"知意输入法",
                    MB_OK | MB_ICONERROR);
        return;
    }
    if (completion->kind == current_user_dict_kind() &&
        current_lexicon_resource() == LexiconResource::kUserLexicon) {
        clear_lexicon_entry_form();
        query_lexicon_entries(false);
        SetWindowTextW(hLexiconStatus_, L"导入完成，正在刷新...");
    }
    MessageBoxW(hwnd_, L"用户词库已导入。", L"知意输入法", MB_OK | MB_ICONINFORMATION);
}

void EditorApp::export_user_dict() {
    if (current_lexicon_resource() != LexiconResource::kUserLexicon) {
        return;
    }
    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, current_user_dict_kind() == UserDictKind::WUBI ? L"user_wubi.tsv"
                                                                  : L"user_pinyin.tsv");
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"TSV 用户词库 (*.tsv)\0*.tsv\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"tsv";
    if (!GetSaveFileNameW(&dialog)) {
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    const bool saved = client.save(current_user_dict_kind(), &result);
    const std::string destination = wstr_to_utf8(file);
    if (!copy_file_utf8_path(current_user_dict_path(), destination)) {
        MessageBoxW(hwnd_, L"导出失败，无法复制词库文件。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    update_lexicon_status();
    std::wstring message = L"用户词库已导出到:\n" + path_for_display(destination);
    if (!saved) {
        message += L"\n\n后台未能立即保存最新内存状态，已导出现有词库文件。";
    }
    MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_OK | MB_ICONINFORMATION);
}

void EditorApp::open_user_dict_dir() {
    const std::wstring directory = path_for_display(user_data_dir());
    const HINSTANCE result =
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        const std::wstring message = L"无法打开用户词库目录:\n" + directory;
        MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_OK | MB_ICONERROR);
    }
}

} // namespace settings
} // namespace cxxime
