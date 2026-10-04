// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <algorithm>
#include <thread>
#include <utility>

#include <cxxime/lexicon_control.h>
#include <cxxime/pipe_names.h>

#include "editor_app_internal.h"
#include "lexicon_query_service.h"

namespace cxxime {
namespace settings {
namespace {

constexpr std::size_t kCandidateOrderQueryLimit = 64;

struct LexiconQueryCompletion {
    UserDictKind kind = UserDictKind::PINYIN;
    LexiconResource resource = LexiconResource::kUserLexicon;
    bool server_available = false;
    bool disabled_state_available = false;
    bool system_query_requested = false;
    bool preserve_editor = false;
    LexiconSearchKind search_kind = LexiconSearchKind::kEmpty;
    std::string query;
    std::vector<LexiconEntryKey> selected_entries;
    std::vector<ManualCandidateOrderEntry> selected_candidate_entries;
    SystemLexiconQueryResult system_result;
    LexiconControlResult server_result;
    CandidateOrderQueryResult candidate_order;
    std::string candidate_order_code;
    std::vector<SystemLexiconEntry> candidate_code_choices;
    std::vector<LexiconPanelEntry> rows;
};

struct LexiconCodeCompletion {
    UserDictKind kind = UserDictKind::PINYIN;
    std::string text;
    std::string error;
    std::vector<std::string> codes;
};

bool control_pipe_is_absent() {
    const std::wstring pipe_name = make_user_pipe_name(CONTROL_PIPE_BASE_NAME);
    return !WaitNamedPipeW(pipe_name.c_str(), 1) && GetLastError() == ERROR_FILE_NOT_FOUND;
}

SystemLexiconType system_lexicon_type(UserDictKind kind) {
    return kind == UserDictKind::WUBI ? SystemLexiconType::kWubi : SystemLexiconType::kPinyin;
}

const wchar_t* source_label(const LexiconPanelEntry& row) {
    if (row.has_system_source && row.has_user_source) {
        return L"系统 + 用户";
    }
    return row.has_system_source ? L"系统" : L"用户";
}

const wchar_t* status_label(const LexiconPanelEntry& row, bool status_available) {
    if (!row.has_system_source) {
        return L"可用";
    }
    if (!status_available) {
        return L"状态未知";
    }
    if (row.system_disabled) {
        return row.has_user_source ? L"系统已停用" : L"已停用";
    }
    return L"可用";
}

const wchar_t* candidate_order_reason_label(CandidateOrderReason reason) {
    switch (reason) {
        case CandidateOrderReason::kManual:
            return L"手动固定";
        case CandidateOrderReason::kLearned:
            return L"自动学习";
        case CandidateOrderReason::kUserLexicon:
            return L"用户词";
        case CandidateOrderReason::kDefault:
        default:
            return L"默认";
    }
}

void set_list_text(HWND list, int row, int column, const std::wstring& text) {
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

std::string selected_combo_text_utf8(HWND combo) {
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return edit_text_utf8(combo);
    }
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, selected, 0);
    if (length == CB_ERR) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(&text[0]));
    text.resize(static_cast<std::size_t>(length));
    return wstr_to_utf8(text);
}

} // namespace

void EditorApp::refresh_lexicon_entries() { query_lexicon_entries(true); }

void EditorApp::query_lexicon_entries(bool preserve_editor) {
    if (!hLexiconList_) {
        return;
    }

    const WPARAM generation = ++lexiconQueryGeneration_;
    const std::string query = edit_text_utf8(hLexiconQuery_);
    const UserDictKind kind = current_user_dict_kind();
    const LexiconResource resource = current_lexicon_resource();
    const std::vector<LexiconEntryKey> selected_entries =
        selected_lexicon_entry_keys(lexiconRows_, selected_lexicon_row_indices());
    std::vector<ManualCandidateOrderEntry> selected_candidate_entries;
    if (resource == LexiconResource::kManualCandidateOrder) {
        for (const std::size_t index : selected_lexicon_row_indices()) {
            if (index < lexiconRows_.size()) {
                const auto& row = lexiconRows_[index];
                selected_candidate_entries.push_back({row.text, row.code, row.syllables});
            }
        }
    }
    const std::string selected_candidate_code =
        resource == LexiconResource::kManualCandidateOrder
            ? selected_combo_text_utf8(hLexiconCode_)
            : std::string();
    LexiconSearchKind search_kind = LexiconSearchKind::kEmpty;
    if (resource == LexiconResource::kUserLexicon ||
        resource == LexiconResource::kManualCandidateOrder) {
        search_kind = classify_lexicon_search(query);
        const bool invalid_query = search_kind == LexiconSearchKind::kInvalid ||
                                (resource == LexiconResource::kManualCandidateOrder &&
                                    search_kind == LexiconSearchKind::kEmpty);
        if (invalid_query) {
            lexiconQueryPending_ = false;
            if (!preserve_editor) {
                clear_lexicon_entry_form();
            } else {
                selectedLexiconText_.clear();
                selectedLexiconCode_.clear();
                selectedLexiconHasSystem_ = false;
                selectedLexiconHasUser_ = false;
                selectedLexiconSystemDisabled_ = false;
                selectedLexiconCount_ = 0;
                selectedLexiconDeletableCount_ = 0;
            }
            ListView_DeleteAllItems(hLexiconList_);
            lexiconRows_.clear();
            lexiconBaseStatus_ = resource == LexiconResource::kManualCandidateOrder
                                     ? L"请输入词语或小写字母编码"
                                     : L"请输入中文词语或小写字母编码";
            SetWindowTextW(hLexiconStatus_, lexiconBaseStatus_.c_str());
            update_lexicon_entry_actions();
            return;
        }
    }

    if (lexiconQueryRunning_) {
        lexiconQueryPending_ = true;
        lexiconQueryPendingPreserveEditor_ = preserve_editor;
        return;
    }
    lexiconQueryRunning_ = true;
    lexiconQueryPending_ = false;

    if (!preserve_editor) {
        clear_lexicon_entry_form();
    }
    ListView_DeleteAllItems(hLexiconList_);
    lexiconRows_.clear();
    update_lexicon_entry_actions();

    const HWND window = hwnd_;
    const bool pipe_absent = control_pipe_is_absent();
    const auto service = lexiconQueryService_;
    SetWindowTextW(hLexiconStatus_, pipe_absent ? L"正在查询系统词典..." : L"正在查询...");

    std::thread([window, generation, kind, resource, search_kind, query, pipe_absent, service,
                 preserve_editor, selected_entries, selected_candidate_entries,
                 selected_candidate_code]() {
        LexiconQueryCompletion completion;
        completion.kind = kind;
        completion.resource = resource;
        completion.preserve_editor = preserve_editor;
        completion.search_kind = search_kind;
        completion.query = query;
        completion.selected_entries = selected_entries;
        completion.selected_candidate_entries = selected_candidate_entries;
        completion.server_available = !pipe_absent;

        if (resource == LexiconResource::kUserLexicon) {
            completion.system_query_requested = search_kind != LexiconSearchKind::kEmpty;
            if (completion.system_query_requested) {
                completion.system_result = service->query(system_lexicon_type(kind), search_kind,
                                                          query, LEXICON_CONTROL_DEFAULT_LIMIT);
            }
        } else if (resource == LexiconResource::kManualCandidateOrder &&
                   search_kind == LexiconSearchKind::kText) {
            completion.system_query_requested = true;
            completion.system_result =
                service->query_exact_text(system_lexicon_type(kind), query, 16);
            for (const auto& entry : completion.system_result.entries) {
                const bool duplicate = std::any_of(
                    completion.candidate_code_choices.begin(),
                    completion.candidate_code_choices.end(),
                    [&](const auto& existing) { return existing.code == entry.code; });
                if (!duplicate) {
                    completion.candidate_code_choices.push_back(entry);
                }
            }
        } else if (resource == LexiconResource::kManualCandidateOrder) {
            completion.candidate_order_code = query;
        }

        auto select_candidate_code = [&]() {
            const auto selected = std::find_if(
                completion.candidate_code_choices.begin(),
                completion.candidate_code_choices.end(), [&](const auto& entry) {
                    return entry.code == selected_candidate_code;
                });
            if (selected != completion.candidate_code_choices.end()) {
                completion.candidate_order_code = selected->code;
            } else if (!completion.candidate_code_choices.empty()) {
                completion.candidate_order_code = completion.candidate_code_choices.front().code;
            }
        };

        std::vector<UserDictEntryInfo> user_entries;
        std::vector<UserDictEntryInfo> disabled_entries;
        if (!pipe_absent) {
            LexiconControlClient client;
            if (resource == LexiconResource::kManualCandidateOrder) {
                if (search_kind == LexiconSearchKind::kText) {
                    LexiconControlResult user_result;
                    if (client.query_exact_user_entries(kind, query, 16, &user_result)) {
                        for (const auto& entry : user_result.query.entries) {
                            if (entry.text != query ||
                                std::any_of(completion.candidate_code_choices.begin(),
                                            completion.candidate_code_choices.end(),
                                            [&](const auto& existing) {
                                                return existing.code == entry.code;
                                            })) {
                                continue;
                            }
                            SystemLexiconEntry choice;
                            choice.text = entry.text;
                            choice.code = entry.code;
                            choice.frequency = entry.frequency;
                            choice.syllables = entry.syllables;
                            choice.user_source = true;
                            completion.candidate_code_choices.push_back(std::move(choice));
                        }
                    }
                    select_candidate_code();
                }
                completion.server_available = !completion.candidate_order_code.empty() &&
                                              client.query_candidate_order(
                                                  kind, completion.candidate_order_code,
                                                  kCandidateOrderQueryLimit,
                                                  &completion.server_result);
                if (completion.server_available) {
                    completion.candidate_order = completion.server_result.candidate_order;
                }
            } else {
                completion.server_available = client.query(
                    resource, kind, query, 0, LEXICON_CONTROL_DEFAULT_LIMIT,
                    &completion.server_result);
                if (completion.server_available) {
                    user_entries = completion.server_result.query.entries;
                }
            }
            if (resource == LexiconResource::kUserLexicon) {
                std::vector<std::string> system_texts;
                for (const auto& entry : completion.system_result.entries) {
                    if (std::find(system_texts.begin(), system_texts.end(), entry.text) ==
                        system_texts.end()) {
                        system_texts.push_back(entry.text);
                    }
                }
                if (system_texts.empty()) {
                    completion.disabled_state_available = completion.server_available;
                } else {
                    LexiconControlResult disabled_result;
                    completion.disabled_state_available =
                        client.query_system_entry_status(kind, system_texts, &disabled_result);
                    if (completion.disabled_state_available) {
                        disabled_entries = std::move(disabled_result.query.entries);
                    }
                }
            }
        }
        if (resource == LexiconResource::kManualCandidateOrder &&
            search_kind == LexiconSearchKind::kText &&
            completion.candidate_order_code.empty()) {
            select_candidate_code();
        }

        if (resource == LexiconResource::kManualCandidateOrder) {
            completion.rows.reserve(completion.candidate_order.entries.size());
            for (const auto& entry : completion.candidate_order.entries) {
                LexiconPanelEntry row;
                row.text = entry.text;
                row.code = entry.code;
                row.syllables = entry.syllables;
                row.candidate_order_reason = entry.reason;
                row.candidate_available = entry.available;
                row.candidate_position_known = entry.available;
                completion.rows.push_back(std::move(row));
            }
            if (search_kind == LexiconSearchKind::kText) {
                const auto exact = std::find_if(
                    completion.candidate_code_choices.begin(),
                    completion.candidate_code_choices.end(), [&](const auto& entry) {
                        return entry.text == query &&
                               entry.code == completion.candidate_order_code;
                    });
                const bool visible = std::any_of(
                    completion.rows.begin(), completion.rows.end(),
                    [&](const auto& row) { return row.text == query; });
                if (exact != completion.candidate_code_choices.end() && !visible) {
                    LexiconPanelEntry row;
                    row.text = exact->text;
                    row.code = exact->code;
                    row.syllables = exact->syllables;
                    row.candidate_order_reason = exact->user_source
                                                     ? CandidateOrderReason::kUserLexicon
                                                     : CandidateOrderReason::kDefault;
                    completion.rows.push_back(std::move(row));
                }
            }
        } else if (resource == LexiconResource::kUserLexicon) {
            completion.rows = merge_lexicon_entries(completion.system_result.entries, user_entries,
                                                    disabled_entries);
            if (completion.rows.size() > LEXICON_CONTROL_DEFAULT_LIMIT) {
                completion.rows.resize(LEXICON_CONTROL_DEFAULT_LIMIT);
            }
        } else {
            completion.rows.reserve(user_entries.size());
            for (const auto& entry : user_entries) {
                LexiconPanelEntry row;
                row.text = entry.text;
                row.code = entry.code;
                row.frequency = entry.frequency;
                row.has_user_source = true;
                completion.rows.push_back(std::move(row));
            }
        }

        SendMessageW(window, kLexiconQueryCompleteMessage, generation,
                     reinterpret_cast<LPARAM>(&completion));
    }).detach();
}

void EditorApp::handle_lexicon_query_complete(WPARAM generation, LPARAM completion_data) {
    const auto* completion = reinterpret_cast<const LexiconQueryCompletion*>(completion_data);
    lexiconQueryRunning_ = false;
    if (lexiconQueryPending_) {
        const bool preserve_editor = lexiconQueryPendingPreserveEditor_;
        lexiconQueryPending_ = false;
        query_lexicon_entries(preserve_editor);
    }
    if (!completion || generation != lexiconQueryGeneration_ ||
        completion->kind != current_user_dict_kind() ||
        completion->resource != current_lexicon_resource()) {
        return;
    }

    lexiconServerAvailable_ = completion->server_available;
    lexiconDisabledStateAvailable_ = completion->disabled_state_available;
    candidateOrderVersion_ = completion->candidate_order.version;
    candidateOrderPins_ = completion->candidate_order.manual_entries;
    candidateOrderCode_ = completion->candidate_order_code;
    if (completion->resource == LexiconResource::kManualCandidateOrder) {
        updatingLexiconForm_ = true;
        SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
        for (const auto& choice : completion->candidate_code_choices) {
            const std::wstring code = utf8_to_wstr(choice.code);
            SendMessageW(hLexiconCode_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(code.c_str()));
        }
        SetWindowTextW(hLexiconCode_, utf8_to_wstr(candidateOrderCode_).c_str());
        updatingLexiconForm_ = false;
    }
    lexiconRows_ = completion->rows;
    ListView_DeleteAllItems(hLexiconList_);
    for (std::size_t index = 0; index < lexiconRows_.size(); ++index) {
        const auto& row_data = lexiconRows_[index];
        const std::wstring text = utf8_to_wstr(row_data.text);
        const std::wstring code = utf8_to_wstr(row_data.code);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        std::wstring first_column = text;
        if (completion->resource == LexiconResource::kManualCandidateOrder) {
            first_column =
                row_data.candidate_position_known ? std::to_wstring(index + 1) : L"--";
        }
        item.pszText = const_cast<LPWSTR>(first_column.c_str());
        const int row = ListView_InsertItem(hLexiconList_, &item);
        if (completion->resource == LexiconResource::kManualCandidateOrder) {
            set_list_text(hLexiconList_, row, 1, text);
            set_list_text(hLexiconList_, row, 2, code);
            set_list_text(hLexiconList_, row, 3,
                          row_data.candidate_available
                              ? candidate_order_reason_label(row_data.candidate_order_reason)
                              : L"当前不可用");
        } else if (completion->resource == LexiconResource::kCandidatePreference) {
            set_list_text(hLexiconList_, row, 1, code);
            wchar_t frequency[32] = {};
            swprintf_s(frequency, L"%d", row_data.frequency);
            set_list_text(hLexiconList_, row, 2, frequency);
        } else {
            set_list_text(hLexiconList_, row, 1, code);
            set_list_text(hLexiconList_, row, 2, source_label(row_data));
            set_list_text(hLexiconList_, row, 3,
                          status_label(row_data, lexiconDisabledStateAvailable_));
        }
    }

    const bool prefill_new_entry = should_prefill_new_lexicon_entry(
        completion->search_kind, completion->query, completion->preserve_editor,
        completion->system_result.available, lexiconRows_);
    if (prefill_new_entry) {
        updatingLexiconForm_ = true;
        SetWindowTextW(hLexiconText_, utf8_to_wstr(completion->query).c_str());
        SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
        SetWindowTextW(hLexiconCode_, L"");
        updatingLexiconForm_ = false;
        lexiconCodeManuallyEdited_ = false;
        request_lexicon_code_suggestions();
    }

    bool selection_restored = false;
    if (completion->preserve_editor &&
        (!completion->selected_entries.empty() ||
         !completion->selected_candidate_entries.empty())) {
        updatingLexiconForm_ = true;
        for (std::size_t index = 0; index < lexiconRows_.size(); ++index) {
            const auto& row = lexiconRows_[index];
            const bool selected =
                completion->resource == LexiconResource::kManualCandidateOrder
                    ? std::any_of(completion->selected_candidate_entries.begin(),
                                  completion->selected_candidate_entries.end(),
                                  [&](const auto& entry) {
                                      return row.text == entry.text &&
                                             row.code == entry.code &&
                                             row.syllables == entry.syllables;
                                  })
                    : std::any_of(completion->selected_entries.begin(),
                                  completion->selected_entries.end(),
                                  [&](const LexiconEntryKey& entry) {
                                      return row.text == entry.text &&
                                             row.code == entry.code;
                                  });
            if (selected) {
                const UINT state = selection_restored ? LVIS_SELECTED
                                                      : LVIS_SELECTED | LVIS_FOCUSED;
                ListView_SetItemState(hLexiconList_, static_cast<int>(index), state,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                selection_restored = true;
            }
        }
        updatingLexiconForm_ = false;
    }
    if (!selection_restored) {
        selectedLexiconText_.clear();
        selectedLexiconCode_.clear();
        selectedLexiconHasSystem_ = false;
        selectedLexiconHasUser_ = false;
        selectedLexiconSystemDisabled_ = false;
        selectedLexiconCount_ = 0;
        selectedLexiconDeletableCount_ = 0;
    }

    if (completion->resource == LexiconResource::kManualCandidateOrder) {
        if (completion->search_kind == LexiconSearchKind::kText &&
            completion->candidate_code_choices.empty() && completion->system_result.available) {
            SetWindowTextW(hLexiconStatus_, L"未找到该词语的可用编码");
        } else if (!completion->server_available) {
            SetWindowTextW(hLexiconStatus_, L"知意输入法后台未运行");
        } else if (lexiconRows_.empty()) {
            SetWindowTextW(hLexiconStatus_, L"该编码没有可用候选");
        } else {
            const std::size_t shown = static_cast<std::size_t>(std::count_if(
                completion->candidate_order.entries.begin(),
                completion->candidate_order.entries.end(),
                [](const auto& entry) { return entry.available; }));
            const std::size_t unavailable = completion->candidate_order.entries.size() - shown;
            wchar_t status[160] = {};
            const wchar_t* more =
                completion->candidate_order.has_more ? L"，还有更多结果" : L"";
            if (unavailable != 0) {
                swprintf_s(status, L"显示 %zu 个候选%s；%zu 个固定项当前不可用", shown, more,
                           unavailable);
            } else {
                swprintf_s(status, L"显示 %zu 个候选%s", shown, more);
            }
            SetWindowTextW(hLexiconStatus_, status);
        }
    } else if (completion->resource == LexiconResource::kCandidatePreference) {
        if (!completion->server_available) {
            SetWindowTextW(hLexiconStatus_, L"知意输入法后台未运行");
        } else if (lexiconRows_.empty()) {
            SetWindowTextW(hLexiconStatus_, L"暂无选词偏好");
        } else {
            wchar_t status[96] = {};
            swprintf_s(status, L"显示 %zu 条选词偏好", lexiconRows_.size());
            SetWindowTextW(hLexiconStatus_, status);
        }
    } else if (completion->system_query_requested && !completion->system_result.available &&
               !completion->server_available) {
        SetWindowTextW(hLexiconStatus_, L"系统词典不可用，知意输入法后台未运行");
    } else if (completion->system_query_requested && !completion->system_result.available) {
        SetWindowTextW(hLexiconStatus_, L"系统词典不可查询，仅显示用户词条");
    } else if (!completion->server_available) {
        SetWindowTextW(hLexiconStatus_,
                       lexiconRows_.empty() ? L"知意输入法后台未运行" : L"仅显示系统词条；后台未运行");
    } else if (lexiconRows_.empty()) {
        SetWindowTextW(hLexiconStatus_,
                       prefill_new_entry ? L"没有匹配词条，可以新增" : L"没有匹配词条");
    } else {
        wchar_t status[96] = {};
        swprintf_s(status, L"显示 %zu 条词条", lexiconRows_.size());
        SetWindowTextW(hLexiconStatus_, status);
    }
    wchar_t base_status[128] = {};
    GetWindowTextW(hLexiconStatus_, base_status, static_cast<int>(_countof(base_status)));
    lexiconBaseStatus_ = base_status;
    if (selection_restored) {
        on_lexicon_selection_changed();
    } else {
        update_lexicon_entry_actions();
    }
}

void EditorApp::request_lexicon_code_suggestions() {
    const WPARAM generation = ++lexiconCodeGeneration_;
    SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
    if (current_lexicon_resource() != LexiconResource::kUserLexicon) {
        return;
    }
    const std::string text = edit_text_utf8(hLexiconText_);
    if (classify_lexicon_search(text) != LexiconSearchKind::kText) {
        lexiconCodePending_ = false;
        return;
    }

    if (lexiconCodeRunning_) {
        lexiconCodePending_ = true;
        return;
    }
    lexiconCodeRunning_ = true;
    lexiconCodePending_ = false;

    const UserDictKind kind = current_user_dict_kind();
    const HWND window = hwnd_;
    const auto service = lexiconQueryService_;
    std::thread([window, generation, kind, text, service]() {
        LexiconCodeCompletion completion;
        completion.kind = kind;
        completion.text = text;
        completion.codes =
            service->suggest_codes(system_lexicon_type(kind), text, 8, &completion.error);
        SendMessageW(window, kLexiconCodeCompleteMessage, generation,
                     reinterpret_cast<LPARAM>(&completion));
    }).detach();
}

void EditorApp::handle_lexicon_code_complete(WPARAM generation, LPARAM completion_data) {
    const auto* completion = reinterpret_cast<const LexiconCodeCompletion*>(completion_data);
    lexiconCodeRunning_ = false;
    if (lexiconCodePending_) {
        lexiconCodePending_ = false;
        request_lexicon_code_suggestions();
    }
    if (!completion || generation != lexiconCodeGeneration_ ||
        completion->kind != current_user_dict_kind() ||
        completion->text != edit_text_utf8(hLexiconText_)) {
        return;
    }

    const std::wstring current_code = utf8_to_wstr(edit_text_utf8(hLexiconCode_));
    updatingLexiconForm_ = true;
    SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
    for (const auto& code : completion->codes) {
        const std::wstring wide_code = utf8_to_wstr(code);
        SendMessageW(hLexiconCode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide_code.c_str()));
    }
    if (!lexiconCodeManuallyEdited_ && !completion->codes.empty()) {
        SetWindowTextW(hLexiconCode_, utf8_to_wstr(completion->codes.front()).c_str());
    } else {
        SetWindowTextW(hLexiconCode_, current_code.c_str());
    }
    updatingLexiconForm_ = false;
    if (completion->codes.empty()) {
        SetWindowTextW(hLexiconStatus_, completion->error.empty()
                                            ? L"未找到可用编码，请手动输入"
                                            : L"系统词典不可用，无法自动生成编码");
    }
    update_lexicon_entry_actions();
}

void EditorApp::clear_lexicon_entry_form() {
    KillTimer(hwnd_, kLexiconCodeTimerId);
    ++lexiconCodeGeneration_;
    lexiconCodePending_ = false;
    selectedLexiconText_.clear();
    selectedLexiconCode_.clear();
    selectedLexiconHasSystem_ = false;
    selectedLexiconHasUser_ = false;
    selectedLexiconSystemDisabled_ = false;
    selectedLexiconCount_ = 0;
    selectedLexiconDeletableCount_ = 0;
    candidateOrderVersion_ = 0;
    candidateOrderPins_.clear();
    candidateOrderCode_.clear();
    lexiconCodeManuallyEdited_ = false;
    updatingLexiconForm_ = true;
    SetWindowTextW(hLexiconText_, L"");
    SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
    SetWindowTextW(hLexiconCode_, L"");
    ListView_SetItemState(hLexiconList_, -1, 0, LVIS_SELECTED);
    updatingLexiconForm_ = false;
    update_lexicon_entry_actions();
}

std::vector<std::size_t> EditorApp::selected_lexicon_row_indices() const {
    std::vector<std::size_t> indices;
    if (!hLexiconList_) {
        return indices;
    }
    int row = -1;
    while ((row = ListView_GetNextItem(hLexiconList_, row, LVNI_SELECTED)) >= 0) {
        indices.push_back(static_cast<std::size_t>(row));
    }
    return indices;
}

void EditorApp::select_all_lexicon_entries() {
    if (!hLexiconList_ || ListView_GetItemCount(hLexiconList_) == 0) {
        return;
    }
    updatingLexiconForm_ = true;
    ListView_SetItemState(hLexiconList_, -1, LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(hLexiconList_, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    updatingLexiconForm_ = false;
    on_lexicon_selection_changed();
}

void EditorApp::on_lexicon_selection_changed() {
    if (updatingLexiconForm_ || !hLexiconList_) {
        return;
    }

    const std::vector<std::size_t> indices = selected_lexicon_row_indices();
    const LexiconSelectionSummary selection = summarize_lexicon_selection(lexiconRows_, indices);
    selectedLexiconCount_ = selection.selected_count;
    selectedLexiconDeletableCount_ = selection.deletable_count;
    selectedLexiconText_.clear();
    selectedLexiconCode_.clear();
    selectedLexiconHasSystem_ = false;
    selectedLexiconHasUser_ = false;
    selectedLexiconSystemDisabled_ = false;
    const bool selection_status_available =
        !lexiconImportRunning_ && !lexiconQueryRunning_ && lexiconServerAvailable_;

    if (selection.selected_count != 1) {
        KillTimer(hwnd_, kLexiconCodeTimerId);
        ++lexiconCodeGeneration_;
        lexiconCodePending_ = false;
        lexiconCodeManuallyEdited_ = false;
        updatingLexiconForm_ = true;
        SetWindowTextW(hLexiconText_, L"");
        SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
        SetWindowTextW(hLexiconCode_, L"");
        updatingLexiconForm_ = false;
        update_lexicon_entry_actions();
        if (selection.selected_count == 0) {
            if (selection_status_available) {
                SetWindowTextW(hLexiconStatus_, lexiconBaseStatus_.c_str());
            }
        } else if (selection_status_available) {
            wchar_t status[96] = {};
            if (selection.deletable_count == selection.selected_count) {
                swprintf_s(status, L"已选择 %zu 项", selection.selected_count);
            } else {
                swprintf_s(status, L"已选择 %zu 项，其中 %zu 项可删除",
                           selection.selected_count, selection.deletable_count);
            }
            SetWindowTextW(hLexiconStatus_, status);
        }
        return;
    }

    const auto& selected = lexiconRows_[selection.first_index];
    selectedLexiconText_ = utf8_to_wstr(selected.text);
    selectedLexiconCode_ = utf8_to_wstr(selected.code);
    selectedLexiconHasSystem_ = selected.has_system_source;
    selectedLexiconHasUser_ = selected.has_user_source;
    selectedLexiconSystemDisabled_ = selected.system_disabled;
    if (current_lexicon_resource() == LexiconResource::kUserLexicon) {
        updatingLexiconForm_ = true;
        SetWindowTextW(hLexiconText_, selectedLexiconText_.c_str());
        SendMessageW(hLexiconCode_, CB_RESETCONTENT, 0, 0);
        SetWindowTextW(hLexiconCode_, selectedLexiconCode_.c_str());
        updatingLexiconForm_ = false;
        lexiconCodeManuallyEdited_ = false;
    }
    update_lexicon_entry_actions();
    if (selection_status_available) {
        SetWindowTextW(hLexiconStatus_, L"已选择 1 项");
    }
}

void EditorApp::update_lexicon_entry_actions() {
    const bool lexicon_page = current_lexicon_resource() == LexiconResource::kUserLexicon;
    const bool candidate_order_page =
        current_lexicon_resource() == LexiconResource::kManualCandidateOrder;
    const bool learning_page =
        current_lexicon_resource() == LexiconResource::kCandidatePreference;
    const bool mutation_available =
        lexiconServerAvailable_ && !lexiconImportRunning_ && !lexiconBatchAddRunning_ &&
        !lexiconQueryRunning_;
    const bool has_text = hLexiconText_ && GetWindowTextLengthW(hLexiconText_) > 0;
    const bool has_code = hLexiconCode_ && GetWindowTextLengthW(hLexiconCode_) > 0;
    EnableWindow(hLexiconAdd_, lexicon_page && mutation_available && has_text);
    EnableWindow(hLexiconSave_, lexicon_page && mutation_available && has_text && has_code &&
                                   selectedLexiconCount_ == 1 && selectedLexiconHasUser_);
    EnableWindow(hLexiconDelete_, lexicon_page && mutation_available &&
                                      selectedLexiconDeletableCount_ > 0);
    EnableWindow(hLexiconPreferenceDelete_,
                 learning_page && mutation_available && selectedLexiconDeletableCount_ > 0);
    EnableWindow(hLexiconSystemAction_, lexicon_page && mutation_available &&
                                            lexiconDisabledStateAvailable_ &&
                                            selectedLexiconCount_ == 1 &&
                                            selectedLexiconHasSystem_);
    SetWindowTextW(hLexiconSystemAction_,
                   selectedLexiconSystemDisabled_ ? L"恢复系统词" : L"隐藏系统词");
    EnableWindow(hLexiconImport_, lexicon_page && mutation_available);
    EnableWindow(hLexiconExport_, lexicon_page && mutation_available);

    const int lexicon_visibility = lexicon_page ? SW_SHOW : SW_HIDE;
    const int preference_visibility = learning_page ? SW_SHOW : SW_HIDE;
    for (HWND control :
         {hLexiconTextLabel_, hLexiconText_, hLexiconCodeLabel_, hLexiconCode_, hLexiconAdd_,
          hLexiconSave_, hLexiconDelete_, hLexiconSystemAction_, hLexiconImport_,
          hLexiconExport_}) {
        ShowWindow(control, lexicon_visibility);
    }
    if (candidate_order_page) {
        SetWindowTextW(hLexiconCodeLabel_, L"编码:");
        ShowWindow(hLexiconCodeLabel_, SW_SHOW);
        ShowWindow(hLexiconCode_, SW_SHOW);
    }
    ShowWindow(hLexiconPreferenceDelete_, preference_visibility);
    ShowWindow(hLexiconPreferenceClear_, preference_visibility);
    EnableWindow(hLexiconPreferenceClear_, learning_page && mutation_available);
    ShowWindow(hLexiconLearningNotice_, preference_visibility);
    if (learning_page) {
        SetWindowTextW(hLexiconLearningNotice_, config_.candidate_learning
                                                    ? L"选词偏好仅用于本机候选排序。"
                                                    : L"已经停止记录偏好；已有偏好不会参与排序。");
    }

    const auto selected_rows = selected_lexicon_row_indices();
    const bool one_candidate_selected = candidate_order_page && selected_rows.size() == 1;
    const LexiconPanelEntry* selected_candidate =
        one_candidate_selected ? &lexiconRows_[selected_rows.front()] : nullptr;
    const auto selected_pin =
        selected_candidate
            ? std::find_if(candidateOrderPins_.begin(), candidateOrderPins_.end(),
                           [&](const auto& entry) {
                               return entry.text == selected_candidate->text &&
                                      entry.code == selected_candidate->code &&
                                      entry.syllables == selected_candidate->syllables;
                           })
            : candidateOrderPins_.end();
    const bool selected_is_manual = selected_pin != candidateOrderPins_.end();
    const bool can_add_candidate =
        selected_is_manual ||
        candidateOrderPins_.size() < MANUAL_CANDIDATE_ORDER_MAX_ENTRIES;
    const std::size_t selected_pin_index = selected_is_manual
            ? static_cast<std::size_t>(std::distance(
                candidateOrderPins_.begin(), selected_pin))
            : 0;
    const int candidate_order_visibility = candidate_order_page ? SW_SHOW : SW_HIDE;
    for (HWND control : {hCandidateOrderFirst_, hCandidateOrderAppend_, hCandidateOrderUp_,
                         hCandidateOrderDown_, hCandidateOrderUnpin_, hCandidateOrderReset_}) {
        ShowWindow(control, candidate_order_visibility);
    }
    const bool selected_candidate_available =
        selected_candidate && selected_candidate->candidate_available;
    EnableWindow(hCandidateOrderFirst_, mutation_available && selected_candidate_available &&
                                             can_add_candidate);
    EnableWindow(hCandidateOrderAppend_, mutation_available && selected_candidate_available &&
                                        !selected_is_manual &&
                                        candidateOrderPins_.size() <
                                            MANUAL_CANDIDATE_ORDER_MAX_ENTRIES);
    EnableWindow(hCandidateOrderUp_, mutation_available && selected_is_manual &&
                                         selected_candidate_available &&
                                         selected_pin_index > 0);
    EnableWindow(hCandidateOrderDown_,
                 mutation_available && selected_is_manual && selected_candidate_available &&
                     selected_pin_index + 1 < candidateOrderPins_.size());
    EnableWindow(hCandidateOrderUnpin_, mutation_available && selected_is_manual);
    const bool has_learned_order =
        std::any_of(lexiconRows_.begin(), lexiconRows_.end(), [](const auto& row) {
            return row.candidate_order_reason == CandidateOrderReason::kLearned;
        });
    EnableWindow(hCandidateOrderReset_, mutation_available && candidate_order_page &&
                                            (!candidateOrderPins_.empty() ||
                                            has_learned_order));

    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT;
    if (candidate_order_page) {
        const wchar_t* headers[] = {L"顺序", L"词语", L"完整编码", L"排序依据"};
        for (int index = 0; index < 4; ++index) {
            column.pszText = const_cast<LPWSTR>(headers[index]);
            ListView_SetColumn(hLexiconList_, index, &column);
        }
        RECT list_rect = {};
        GetClientRect(hLexiconList_, &list_rect);
        ListView_SetColumnWidth(hLexiconList_, 0, S(52));
        ListView_SetColumnWidth(hLexiconList_, 1, S(150));
        ListView_SetColumnWidth(hLexiconList_, 2, S(110));
        ListView_SetColumnWidth(hLexiconList_, 3,
                                (std::max)(S(100), static_cast<int>(list_rect.right) - S(312)));
        return;
    }
    RECT list_rect = {};
    GetClientRect(hLexiconList_, &list_rect);
    ListView_SetColumnWidth(hLexiconList_, 0, S(170));
    ListView_SetColumnWidth(hLexiconList_, 1, S(145));
    ListView_SetColumnWidth(hLexiconList_, 2, S(105));
    ListView_SetColumnWidth(
        hLexiconList_, 3, (std::max)(S(48), static_cast<int>(list_rect.right) - S(420)));
    column.pszText = const_cast<LPWSTR>(L"词语");
    ListView_SetColumn(hLexiconList_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"编码");
    ListView_SetColumn(hLexiconList_, 1, &column);
    column.pszText = const_cast<LPWSTR>(lexicon_page ? L"来源" : L"选择次数");
    ListView_SetColumn(hLexiconList_, 2, &column);
    column.pszText = const_cast<LPWSTR>(lexicon_page ? L"状态" : L"");
    ListView_SetColumn(hLexiconList_, 3, &column);
}

} // namespace settings
} // namespace cxxime
