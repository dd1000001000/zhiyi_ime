// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cxxime/lexicon_control.h>
#include <cxxime/user_dict_validation.h>

#include "editor_app_internal.h"
#include "lexicon_query_service.h"

namespace cxxime {
namespace settings {
namespace {

enum class BatchAddOutcome {
    kAdded,
    kExisted,
    kNoCode,
    kFailed,
};

struct BatchAddCompletion {
    std::shared_ptr<const bool> token;
    std::string text;
    std::string form_code;
    UserDictKind form_kind = UserDictKind::PINYIN;
    struct Target {
        UserDictKind kind = UserDictKind::PINYIN;
        BatchAddOutcome outcome = BatchAddOutcome::kFailed;
    };
    std::vector<Target> targets;
};

SystemLexiconType system_lexicon_type(UserDictKind kind) {
    return kind == UserDictKind::WUBI ? SystemLexiconType::kWubi : SystemLexiconType::kPinyin;
}

BatchAddOutcome add_batch_target(LexiconControlClient* client, LexiconQueryService* query_service,
                                 UserDictKind kind, const std::string& text,
                                 const std::string& preferred_code,
                                 const std::string& preferred_syllables) {
    LexiconControlResult result;
    if (!client->query_exact_user_entries(kind, text, LEXICON_CONTROL_MAX_LIMIT, &result)) {
        return BatchAddOutcome::kFailed;
    }
    for (const auto& entry : result.query.entries) {
        if (entry.text == text) {
            return BatchAddOutcome::kExisted;
        }
    }
    std::string code;
    std::string syllables;
    if (is_valid_user_dict_entry(text, preferred_code, preferred_syllables)) {
        code = preferred_code;
        syllables = preferred_syllables;
    } else {
        std::string error;
        const auto suggestions =
            query_service->suggest_codes(system_lexicon_type(kind), text, 1, &error);
        if (!error.empty()) {
            return BatchAddOutcome::kFailed;
        }
        if (!suggestions.empty() && is_valid_user_dict_entry(text, suggestions.front())) {
            code = suggestions.front();
        }
    }
    if (code.empty()) {
        return BatchAddOutcome::kNoCode;
    }
    return client->add_entry(kind, text, code, &result, syllables) ? BatchAddOutcome::kAdded
                                                                   : BatchAddOutcome::kFailed;
}

const wchar_t* kind_label(UserDictKind kind) {
    return kind == UserDictKind::WUBI ? L"五笔" : L"拼音";
}

const wchar_t* outcome_text(const BatchAddCompletion::Target& target) {
    switch (target.outcome) {
    case BatchAddOutcome::kAdded:
        return L"已添加";
    case BatchAddOutcome::kExisted:
        return L"已存在";
    case BatchAddOutcome::kNoCode:
        return L"无法生成编码";
    case BatchAddOutcome::kFailed:
    default:
        return L"添加失败";
    }
}

bool target_satisfied(const BatchAddCompletion::Target& target) {
    return target.outcome == BatchAddOutcome::kAdded ||
           target.outcome == BatchAddOutcome::kExisted;
}

} // namespace

bool EditorApp::normalize_lexicon_entry_code(UserDictKind kind, const std::string& input,
                                             std::string* code, std::string* syllables) {
    if (kind == UserDictKind::WUBI) {
        if (!code || !syllables || !is_valid_user_dict_code(input)) {
            return false;
        }
        *code = input;
        syllables->clear();
        return true;
    }
    std::string error;
    return lexiconQueryService_ && lexiconQueryService_->normalize_pinyin_code(
                                       input, selected_pinyin_scheme_id(), code, syllables, &error);
}

void EditorApp::add_lexicon_entry() {
    const std::string text = edit_text_utf8(hLexiconText_);
    const std::string form_code = edit_text_utf8(hLexiconCode_);
    if (text.empty() || form_code.empty()) {
        MessageBoxW(hwnd_, L"请输入词语和编码。", L"知意输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string code;
    std::string syllables;
    if (!normalize_lexicon_entry_code(current_user_dict_kind(), form_code, &code, &syllables)) {
        MessageBoxW(hwnd_, L"请输入有效的全拼、当前方案双拼或五笔编码。", L"知意输入法",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (selectedLexiconHasUser_ && text == wstr_to_utf8(selectedLexiconText_) &&
        code == wstr_to_utf8(selectedLexiconCode_)) {
        MessageBoxW(hwnd_, L"当前用户词已经存在。", L"知意输入法", MB_OK | MB_ICONINFORMATION);
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    if (!client.add_entry(current_user_dict_kind(), text, code, &result, syllables)) {
        MessageBoxW(hwnd_, L"新增词条失败。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    clear_lexicon_entry_form();
    query_lexicon_entries(false);
}

void EditorApp::add_lexicon_entry_to_both() {
    const std::string text = edit_text_utf8(hLexiconText_);
    if (!is_valid_user_dict_text(text)) {
        MessageBoxW(hwnd_, L"请输入有效的词语。", L"知意输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::string form_code = edit_text_utf8(hLexiconCode_);
    std::string current_code;
    std::string current_syllables;
    if (!form_code.empty() &&
        !normalize_lexicon_entry_code(current_user_dict_kind(), form_code, &current_code,
                                      &current_syllables)) {
        MessageBoxW(hwnd_, L"请输入有效的编码。", L"知意输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!lexiconQueryService_ || lexiconBatchAddRunning_) {
        return;
    }

    KillTimer(hwnd_, kLexiconCodeTimerId);
    ++lexiconCodeGeneration_;
    lexiconCodePending_ = false;
    lexiconBatchAddRunning_ = true;
    update_lexicon_entry_actions();
    SetWindowTextW(hLexiconStatus_, L"正在添加到拼音和五笔...");
    const UserDictKind current_kind = current_user_dict_kind();
    const HWND window = hwnd_;
    const auto query_service = lexiconQueryService_;
    const auto token = lexiconBatchAddToken_;
    std::thread([window, text, form_code, current_code, current_syllables, current_kind,
                 query_service, token]() {
        BatchAddCompletion completion;
        completion.token = token;
        completion.text = text;
        completion.form_code = form_code;
        completion.form_kind = current_kind;
        LexiconControlClient client;
        completion.targets = {
            {UserDictKind::PINYIN,
             add_batch_target(&client, query_service.get(), UserDictKind::PINYIN, text,
                              current_kind == UserDictKind::PINYIN ? current_code
                                                                   : std::string(),
                              current_kind == UserDictKind::PINYIN ? current_syllables
                                                                   : std::string())},
            {UserDictKind::WUBI,
             add_batch_target(&client, query_service.get(), UserDictKind::WUBI, text,
                              current_kind == UserDictKind::WUBI ? current_code
                                                                 : std::string(),
                              std::string())},
        };
        SendMessageW(window, kLexiconBatchAddCompleteMessage, 0,
                     reinterpret_cast<LPARAM>(&completion));
    }).detach();
}

void EditorApp::handle_lexicon_batch_add_complete(LPARAM completion_data) {
    const auto* completion = reinterpret_cast<const BatchAddCompletion*>(completion_data);
    if (!completion || completion->token != lexiconBatchAddToken_) {
        return;
    }
    lexiconBatchAddRunning_ = false;
    update_lexicon_entry_actions();
    const bool form_unchanged =
        completion->text == edit_text_utf8(hLexiconText_) &&
        completion->form_code == edit_text_utf8(hLexiconCode_) &&
        completion->form_kind == current_user_dict_kind();
    const bool all_satisfied =
        std::all_of(completion->targets.begin(), completion->targets.end(), target_satisfied);
    const bool all_existed = std::all_of(completion->targets.begin(), completion->targets.end(),
                                         [](const BatchAddCompletion::Target& target) {
                                             return target.outcome == BatchAddOutcome::kExisted;
                                         });
    std::wstring message = L"部分用户词未添加\n";
    for (const auto& target : completion->targets) {
        message += kind_label(target.kind);
        message += L"：";
        message += outcome_text(target);
        message += L"\n";
    }
    if (all_existed && form_unchanged) {
        SetWindowTextW(hLexiconStatus_, L"拼音和五笔用户词均已存在");
    } else if (all_satisfied && form_unchanged) {
        clear_lexicon_entry_form();
        query_lexicon_entries(false);
    } else if (form_unchanged) {
        query_lexicon_entries(true);
    } else {
        SetWindowTextW(hLexiconStatus_,
                       all_satisfied ? L"添加完成，当前输入内容已保留"
                                     : L"部分用户词未添加，输入内容已保留");
    }
    if (!all_satisfied) {
        MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_OK | MB_ICONWARNING);
    }
}

void EditorApp::save_lexicon_entry() {
    if (!selectedLexiconHasUser_) {
        return;
    }
    const std::string text = edit_text_utf8(hLexiconText_);
    const std::string form_code = edit_text_utf8(hLexiconCode_);
    if (text.empty() || form_code.empty()) {
        MessageBoxW(hwnd_, L"请输入词语和编码。", L"知意输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string code;
    std::string syllables;
    if (!normalize_lexicon_entry_code(current_user_dict_kind(), form_code, &code, &syllables)) {
        MessageBoxW(hwnd_, L"请输入有效的全拼、当前方案双拼或五笔编码。", L"知意输入法",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    if (!client.replace_entry(current_user_dict_kind(), wstr_to_utf8(selectedLexiconText_),
                              wstr_to_utf8(selectedLexiconCode_), text, code, &result, syllables)) {
        MessageBoxW(hwnd_, L"保存修改失败，可能存在相同的用户词条。", L"知意输入法",
                    MB_OK | MB_ICONERROR);
        return;
    }
    clear_lexicon_entry_form();
    query_lexicon_entries(false);
}

void EditorApp::delete_lexicon_entries() {
    if (!lexiconServerAvailable_ || lexiconImportRunning_ || lexiconQueryRunning_) {
        return;
    }
    const std::vector<LexiconEntryKey> entries =
        selected_user_entry_keys(lexiconRows_, selected_lexicon_row_indices());
    if (entries.empty()) {
        return;
    }
    const bool preference = current_lexicon_resource() == LexiconResource::kCandidatePreference;
    wchar_t count[32] = {};
    swprintf_s(count, L"%zu", entries.size());
    std::wstring message = L"删除选中的 ";
    message += count;
    message += preference ? L" 项选词偏好？" : L" 个用户词？";
    if (!preference && selectedLexiconCount_ > entries.size()) {
        wchar_t retained[32] = {};
        swprintf_s(retained, L"%zu", selectedLexiconCount_ - entries.size());
        message += L"\n\n另外 ";
        message += retained;
        message += L" 个仅系统词不会受影响。";
    }
    if (MessageBoxW(hwnd_, message.c_str(), L"知意输入法", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    const bool deleted = preference
                             ? client.delete_preferences(current_user_dict_kind(), entries, &result)
                             : client.delete_entries(current_user_dict_kind(), entries, &result);
    if (!deleted) {
        MessageBoxW(hwnd_, L"删除词条失败。", L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    clear_lexicon_entry_form();
    query_lexicon_entries(false);
}

void EditorApp::disable_or_restore_system_entry() {
    if (!selectedLexiconHasSystem_ || !lexiconDisabledStateAvailable_) {
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    const std::string text = wstr_to_utf8(selectedLexiconText_);
    const bool succeeded =
        selectedLexiconSystemDisabled_
            ? client.restore_system_entry(current_user_dict_kind(), text, &result)
            : client.disable_system_entry(current_user_dict_kind(), text, &result);
    if (!succeeded) {
        MessageBoxW(hwnd_,
                    selectedLexiconSystemDisabled_ ? L"恢复系统词失败。" : L"隐藏系统词失败。",
                    L"知意输入法", MB_OK | MB_ICONERROR);
        return;
    }
    query_lexicon_entries(false);
}

} // namespace settings
} // namespace cxxime
