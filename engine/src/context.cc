// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/context.h>

#include <algorithm>
#include <limits>
#include <tuple>

#include <windows.h>

#include <cxxime/key_event.h>
#include <cxxime/symbol_table.h>

namespace cxxime {

namespace {

bool is_resumable_ime_code(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
}

} // namespace

const CandidateEntry* Context::candidate_entry(int index) const {
    if (index < 0 || index >= candidate_count()) {
        return nullptr;
    }
    return &translation_.entries[static_cast<std::size_t>(index)];
}

const Candidate* Context::candidate(int index) const {
    const CandidateEntry* entry = candidate_entry(index);
    return entry ? &entry->candidate : nullptr;
}

bool Context::set_ime_scheme(CompositionScheme scheme) {
    if (scheme == CompositionScheme::kSymbol || scheme == CompositionScheme::kInlineAscii) {
        return false;
    }
    if (!composition_.set_scheme(scheme)) {
        return false;
    }
    ime_scheme_ = scheme;
    return true;
}

bool Context::set_preedit(std::string text) {
    const std::size_t cursor = text.size();
    return composition_.set_active_input(std::move(text), cursor);
}

bool Context::start_composition(CompositionScheme scheme, std::string text,
                                std::size_t cursor) {
    if (text.empty() || cursor > text.size() ||
        (scheme == CompositionScheme::kSymbol && (text.front() != kSymbolPrefix || cursor == 0))) {
        return false;
    }
    CompositionState next = composition_;
    next.cancel();
    if (!next.set_scheme(scheme) || !next.set_active_input(std::move(text), cursor)) {
        return false;
    }
    composition_ = std::move(next);
    if (scheme != CompositionScheme::kSymbol && scheme != CompositionScheme::kInlineAscii) {
        ime_scheme_ = scheme;
    }
    composition_origin_.reset();
    commit_source_ = scheme == CompositionScheme::kInlineAscii
                         ? CommitSource::kRawCodePreserveCase
                         : CommitSource::kRawCode;
    return true;
}

bool Context::enter_inline_ascii(bool preserve_origin) {
    if (!is_composing() || composition_scheme() == CompositionScheme::kInlineAscii) {
        return false;
    }
    if (preserve_origin) {
        composition_origin_ =
            CompositionOrigin{composition_scheme(), active_input(), preedit_cursor()};
    } else {
        composition_origin_.reset();
    }
    if (!composition_.set_scheme(CompositionScheme::kInlineAscii)) {
        return false;
    }
    commit_source_ = CommitSource::kRawCodePreserveCase;
    return true;
}

bool Context::restore_composition_origin() {
    if (composition_scheme() != CompositionScheme::kInlineAscii || !composition_origin_) {
        return false;
    }
    const CompositionOrigin origin = *composition_origin_;
    const bool original_input_restored = active_input() == origin.input;
    if (!original_input_restored &&
        (origin.scheme == CompositionScheme::kSymbol || !is_resumable_ime_code(active_input()))) {
        return false;
    }
    const std::size_t edited_cursor = preedit_cursor();
    if (!composition_.set_scheme(origin.scheme)) {
        return false;
    }
    composition_origin_.reset();
    const std::size_t cursor = original_input_restored
                                   ? (std::min)(origin.cursor, active_input().size())
                                   : edited_cursor;
    if (!composition_.set_active_input(active_input(), cursor)) {
        return false;
    }
    commit_source_ = CommitSource::kRawCode;
    return true;
}

void Context::clear_preedit() {
    composition_.cancel();
    composition_.set_scheme(ime_scheme_);
    composition_origin_.reset();
}

bool Context::insert_preedit(char ch) {
    return composition_.insert(ch);
}

bool Context::erase_preedit_before_cursor() {
    return composition_.erase_before_cursor();
}

bool Context::erase_preedit_at_cursor() {
    return composition_.erase_at_cursor();
}

bool Context::move_preedit_cursor_left() {
    return composition_.move_cursor_left();
}

bool Context::move_preedit_cursor_right() {
    return composition_.move_cursor_right();
}

bool Context::move_preedit_cursor_home() {
    return composition_.move_cursor_home();
}

bool Context::move_preedit_cursor_end() {
    return composition_.move_cursor_end();
}

bool Context::edit_preedit(const KeyEvent& event) {
    if (event.is_key_up || event.is_ctrl() || event.is_alt() || !is_composing()) {
        return false;
    }

    bool handled = true;
    switch (event.keycode) {
    case VK_BACK:
        erase_preedit_before_cursor();
        break;
    case VK_DELETE:
        erase_preedit_at_cursor();
        break;
    case VK_LEFT:
        move_preedit_cursor_left();
        break;
    case VK_RIGHT:
        move_preedit_cursor_right();
        break;
    case VK_HOME:
        move_preedit_cursor_home();
        break;
    case VK_END:
        move_preedit_cursor_end();
        break;
    default:
        handled = false;
        break;
    }
    if (!handled) {
        return false;
    }
    if (!is_composing()) {
        clear_translation();
    }
    return true;
}

void Context::reset() {
    clear_preedit();
    committed_text.clear();
    clear_translation();
    reset_pagination();
    commit_source_ = CommitSource::kRawCode;
    commit_learning_plan_ = {};
    requested_candidate_index_.reset();
    requested_navigation_.reset();
    continuation_effort_ = 0;
}

bool Context::commit_candidate(int index) {
    const CandidateEntry* entry = candidate_entry(index);
    return entry && commit_entry(*entry);
}

bool Context::commit_entry(const CandidateEntry& entry) {
    const auto* action = std::get_if<TextSelectionAction>(&entry.selection);
    if (!action || action->consumed_input_bytes != active_input().size()) {
        return false;
    }
    CommitLearningPlan learning_plan = make_candidate_learning_plan(composition_, *action);
    if (!commit_selection(*action)) {
        return false;
    }
    commit_learning_plan_ = std::move(learning_plan);
    return true;
}

bool Context::commit_selection(const TextSelectionAction& action) {
    if (!composition_.finalize_candidate(action, committed_text)) {
        return false;
    }
    composition_.set_scheme(ime_scheme_);
    commit_source_ = CommitSource::kCandidate;
    clear_translation();
    composition_origin_.reset();
    return true;
}

bool Context::finalize_raw(CommitSource source) {
    CommitLearningPlan learning_plan = make_raw_learning_plan(composition_);
    if (!composition_.finalize_raw(committed_text)) {
        return false;
    }
    composition_.set_scheme(ime_scheme_);
    commit_source_ = source;
    clear_translation();
    composition_origin_.reset();
    commit_learning_plan_ = std::move(learning_plan);
    return true;
}

bool Context::request_candidate_selection(int index) {
    if (!candidate_entry(index) || index >= selectable_candidate_count()) {
        return false;
    }
    requested_candidate_index_ = index;
    return true;
}

std::optional<int> Context::take_requested_candidate_selection() {
    const std::optional<int> requested = requested_candidate_index_;
    requested_candidate_index_.reset();
    return requested;
}

CommitLearningPlan Context::take_commit_learning_plan() {
    CommitLearningPlan plan = std::move(commit_learning_plan_);
    commit_learning_plan_ = {};
    return plan;
}

std::string Context::commit() {
    std::string text;
    CommitSource source;
    std::tie(text, source) = commit_with_source();
    return text;
}

std::pair<std::string, CommitSource> Context::commit_with_source() {
    std::string text;
    CommitSource source = commit_source_;
    bool finalized = false;
    bool generated_learning_plan = false;
CommitLearningPlan learning_plan;
    if (!committed_text.empty()) {
        text = std::move(committed_text);
        finalized = true;
    } else {
        bool finalized_candidate = false;
        const CandidateEntry* entry = candidate_entry(highlighted());
        const CompositionScheme scheme = composition_scheme();
        const auto* action =
            entry ? std::get_if<TextSelectionAction>(&entry->selection) : nullptr;
        if (action) {
            if (action->consumed_input_bytes == active_input().size()) {
                learning_plan = make_candidate_learning_plan(composition_, *action);
            }
            if (action->consumed_input_bytes == active_input().size() &&
                composition_.finalize_candidate(*action, text)) {
                source = CommitSource::kCandidate;
                finalized_candidate = true;
                finalized = true;
                generated_learning_plan = true;
            } else if (action->consumed_input_bytes < active_input().size()) {
                CompositionState next = composition_;
                learning_plan = make_partial_raw_learning_plan(composition_, *action);
                if (next.confirm_prefix(*action) && next.finalize_raw(text)) {
                    composition_ = std::move(next);
                    source = CommitSource::kCandidate;
                    finalized_candidate = true;
                    finalized = true;
                    generated_learning_plan = true;
                }
            }
        }
        if (!finalized_candidate) {
            learning_plan = make_raw_learning_plan(composition_);
        }
        if (!finalized_candidate && composition_.finalize_raw(text)) {
            source = scheme == CompositionScheme::kInlineAscii
                         ? CommitSource::kRawCodePreserveCase
                         : CommitSource::kRawCode;
            finalized = true;
            generated_learning_plan = true;
        }
    }
    if (!finalized) {
        return {{}, source};
    }
    if (!composition_.is_composing()) {
        composition_.set_scheme(ime_scheme_);
    }
    committed_text.clear();
    clear_translation();
    reset_pagination();
    commit_source_ = CommitSource::kRawCode;
    composition_origin_.reset();
    if (generated_learning_plan) {
        commit_learning_plan_ = std::move(learning_plan);
    }
    return {std::move(text), source};
}

void Context::update_candidates(CandidatePage&& page) {
    update_translation(make_translation_result(std::move(page), active_input().size()));
}

void Context::update_translation(TranslationResult&& result) {
    const int highlight_count = highlight_count_after_page_change_;
    highlight_count_after_page_change_ = 0;
    translation_ = std::move(result);
    if (translation_.extent.complete) {
        continuation_effort_ = 0;
    }
    const int selectable_count = selectable_candidate_count();
    const int count = candidate_count();
    if (count <= 0) {
        translation_.highlighted = -1;
    } else if (highlight_count > 0) {
        translation_.highlighted = (std::min)(highlight_count, count) - 1;
    } else if (selectable_count <= 0 || translation_.highlighted < 0 ||
               translation_.highlighted >= selectable_count) {
        translation_.highlighted = 0;
    }
}

void Context::clear_translation() {
    translation_ = {};
    requested_navigation_.reset();
    continuation_effort_ = 0;
}

void Context::replace_composition(CompositionState&& state, TranslationResult&& result) {
    composition_ = std::move(state);
    previous_pages_.clear();
    highlight_count_after_page_change_ = 0;
    visible_candidate_count = 0;
    requested_navigation_.reset();
    continuation_effort_ = 0;
    update_translation(std::move(result));
}

void Context::reset_pagination() {
    translation_.page_index = 0;
    translation_.page_offset = 0;
    visible_candidate_count = 0;
    previous_pages_.clear();
    highlight_count_after_page_change_ = 0;
    requested_navigation_.reset();
    continuation_effort_ = 0;
}

int Context::selectable_candidate_count() const {
    int count = candidate_count();
    if (visible_candidate_count > 0) {
        count = (std::min)(count, visible_candidate_count);
    }
    return count;
}

void Context::request_next_page() {
    if (selectable_candidate_count() <= 0) {
        return;
    }
    requested_navigation_ = {CandidateNavigation::kNextPage, false};
}

void Context::request_previous_page(bool highlight_last) {
    if (previous_pages_.empty()) {
        return;
    }
    requested_navigation_ = {CandidateNavigation::kPreviousPage, highlight_last};
}

std::optional<CandidateNavigationRequest> Context::take_candidate_navigation() {
    std::optional<CandidateNavigationRequest> request = requested_navigation_;
    requested_navigation_.reset();
    return request;
}

bool Context::apply_next_page(TranslationResult&& result, int visible_count) {
    if (visible_count <= 0 || result.entries.empty() ||
        result.page_index != translation_.page_index + 1 ||
        result.page_offset != translation_.page_offset + visible_count) {
        return false;
    }
    PageHistoryEntry history;
    history.translation = std::move(translation_);
    history.visible_candidate_count = visible_count;
    history.continuation_effort = continuation_effort_;
    previous_pages_.push_back(std::move(history));
    update_translation(std::move(result));
    return true;
}

bool Context::apply_previous_page(bool highlight_last) {
    if (previous_pages_.empty()) {
        return false;
    }
    PageHistoryEntry history = std::move(previous_pages_.back());
    highlight_count_after_page_change_ = highlight_last ? history.visible_candidate_count : 0;
    continuation_effort_ = history.continuation_effort;
    previous_pages_.pop_back();
    update_translation(std::move(history.translation));
    return true;
}

void Context::update_current_extent(CandidateExtent extent) {
    const int current_end =
        translation_.page_offset + static_cast<int>(translation_.entries.size());
    translation_.extent.known_count =
        extent.state == CandidateExtentState::kExhausted
            ? (std::max)(current_end, extent.known_count)
            : (std::max)(translation_.extent.known_count, extent.known_count);
    translation_.extent.state = extent.state;
    translation_.extent.complete = extent.complete;
    if (extent.complete) {
        continuation_effort_ = 0;
    }
}

uint32_t Context::begin_candidate_continuation() {
    if (continuation_effort_ != (std::numeric_limits<uint32_t>::max)()) {
        ++continuation_effort_;
    }
    return continuation_effort_;
}

bool Context::candidate_was_published(const CandidateEntry& entry,
                                      int current_visible_count) const {
    const auto page_contains = [&](const TranslationResult& page, int visible_count) {
        const int count =
            (std::min)((std::max)(0, visible_count), static_cast<int>(page.entries.size()));
        return std::any_of(page.entries.begin(), page.entries.begin() + count,
                           [&](const CandidateEntry& published) {
                               return same_candidate_entry_identity(published, entry);
                           });
    };
    for (const PageHistoryEntry& history : previous_pages_) {
        if (page_contains(history.translation, history.visible_candidate_count)) {
            return true;
        }
    }
    return page_contains(translation_, current_visible_count);
}

void Context::replace_candidate_sequence(TranslationResult&& result) {
    previous_pages_.clear();
    visible_candidate_count = 0;
    continuation_effort_ = 0;
    update_translation(std::move(result));
}

void Context::move_to_next_candidate() {
    const int count = selectable_candidate_count();
    if (count <= 0) {
        return;
    }
    if (translation_.highlighted < count - 1) {
        ++translation_.highlighted;
        return;
    }
    request_next_page();
}

void Context::move_to_previous_candidate() {
    if (selectable_candidate_count() <= 0) {
        return;
    }
    if (translation_.highlighted > 0) {
        --translation_.highlighted;
        return;
    }
    request_previous_page(true);
}

void Context::cycle_highlight(bool backward) {
    const int count = selectable_candidate_count();
    if (count <= 0) {
        return;
    }
    const int current = translation_.highlighted;
    if (current < 0 || current >= count) {
        translation_.highlighted = backward ? count - 1 : 0;
    } else {
        translation_.highlighted = (current + (backward ? count - 1 : 1)) % count;
    }
}

} // namespace cxxime
