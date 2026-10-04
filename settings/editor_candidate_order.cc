// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <algorithm>

#include <windows.h>

#include <cxxime/lexicon_control.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

ManualCandidateOrderEntry to_manual_entry(const LexiconPanelEntry& row) {
    return {row.text, row.code, row.syllables};
}

bool same_candidate(const ManualCandidateOrderEntry& entry, const LexiconPanelEntry& row) {
    return entry.text == row.text && entry.code == row.code && entry.syllables == row.syllables;
}

} // namespace

bool EditorApp::save_candidate_order(const std::vector<ManualCandidateOrderEntry>& entries) {
    const std::string code = candidateOrderCode_;
    if (code.empty()) {
        return false;
    }

    LexiconControlClient client;
    LexiconControlResult result;
    if (!client.set_candidate_order(current_user_dict_kind(), code, entries, candidateOrderVersion_,
                                    &result)) {
        const wchar_t* message = result.error_code == ERROR_REVISION_MISMATCH
                                     ? L"候选顺序已在其他位置更新，请刷新后重试。"
                                     : L"保存候选顺序失败。";
        MessageBoxW(hwnd_, message, L"知意输入法", MB_OK | MB_ICONERROR);
        query_lexicon_entries(false);
        return false;
    }
    candidateOrderVersion_ = result.candidate_order.version;
    query_lexicon_entries(false);
    return true;
}

void EditorApp::pin_candidate_order_first() {
    const auto selected = selected_lexicon_row_indices();
    if (current_lexicon_resource() != LexiconResource::kManualCandidateOrder ||
        selected.size() != 1) {
        return;
    }

    const auto& selected_row = lexiconRows_[selected.front()];
    if (!selected_row.candidate_available) {
        return;
    }
    std::vector<ManualCandidateOrderEntry> entries = candidateOrderPins_;
    const bool already_pinned =
        std::any_of(entries.begin(), entries.end(),
                    [&](const auto& entry) { return same_candidate(entry, selected_row); });
    if (!already_pinned && entries.size() >= MANUAL_CANDIDATE_ORDER_MAX_ENTRIES) {
        return;
    }
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                                 [&](const auto& entry) {
                                     return same_candidate(entry, selected_row);
                                 }),
                  entries.end());
    entries.insert(entries.begin(), to_manual_entry(selected_row));
    save_candidate_order(entries);
}

void EditorApp::append_candidate_order_pin() {
    const auto selected = selected_lexicon_row_indices();
    if (current_lexicon_resource() != LexiconResource::kManualCandidateOrder ||
        selected.size() != 1) {
        return;
    }
    const auto& selected_row = lexiconRows_[selected.front()];
    if (!selected_row.candidate_available ||
        candidateOrderPins_.size() >= MANUAL_CANDIDATE_ORDER_MAX_ENTRIES ||
        std::any_of(candidateOrderPins_.begin(), candidateOrderPins_.end(),
                    [&](const auto& entry) { return same_candidate(entry, selected_row); })) {
        return;
    }
    auto entries = candidateOrderPins_;
    entries.push_back(to_manual_entry(selected_row));
    save_candidate_order(entries);
}

void EditorApp::move_candidate_order(int direction) {
    const auto selected = selected_lexicon_row_indices();
    if (current_lexicon_resource() != LexiconResource::kManualCandidateOrder ||
        selected.size() != 1 || (direction != -1 && direction != 1)) {
        return;
    }

    const auto& selected_row = lexiconRows_[selected.front()];
    std::vector<ManualCandidateOrderEntry> entries = candidateOrderPins_;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return same_candidate(entry, selected_row);
    });
    if (found == entries.end()) {
        return;
    }
    const std::ptrdiff_t index = std::distance(entries.begin(), found);
    const std::ptrdiff_t next = index + direction;
    if (next < 0 || next >= static_cast<std::ptrdiff_t>(entries.size())) {
        return;
    }
    std::iter_swap(entries.begin() + index, entries.begin() + next);
    save_candidate_order(entries);
}

void EditorApp::remove_candidate_order_pin() {
    const auto selected = selected_lexicon_row_indices();
    if (current_lexicon_resource() != LexiconResource::kManualCandidateOrder ||
        selected.size() != 1) {
        return;
    }

    const auto& selected_row = lexiconRows_[selected.front()];
    std::vector<ManualCandidateOrderEntry> entries = candidateOrderPins_;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                                 [&](const auto& entry) {
                                     return same_candidate(entry, selected_row);
                                 }),
                  entries.end());
    save_candidate_order(entries);
}

void EditorApp::reset_candidate_order() {
    if (current_lexicon_resource() != LexiconResource::kManualCandidateOrder ||
        candidateOrderCode_.empty() ||
        MessageBoxW(hwnd_, L"恢复此编码的默认候选顺序？", L"知意输入法", MB_YESNO | MB_ICONWARNING) !=
            IDYES) {
        return;
    }
    const std::string code = candidateOrderCode_;
    LexiconControlClient client;
    LexiconControlResult result;
    if (!client.clear_candidate_order(current_user_dict_kind(), code, candidateOrderVersion_,
                                      &result)) {
        const wchar_t* message = result.error_code == ERROR_REVISION_MISMATCH
                                     ? L"候选顺序已在其他位置更新，请刷新后重试。"
                                     : L"恢复默认候选顺序失败。";
        MessageBoxW(hwnd_, message, L"知意输入法", MB_OK | MB_ICONERROR);
    }
    query_lexicon_entries(false);
}

} // namespace settings
} // namespace cxxime
