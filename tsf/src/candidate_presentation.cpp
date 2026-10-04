// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "candidate_presentation.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace cxxime_tsf {

namespace {

bool same_caret_position(const RECT& left, const RECT& right) {
    constexpr LONG kTolerancePx = 2;
    return std::abs(left.left - right.left) <= kTolerancePx &&
           std::abs(left.top - right.top) <= kTolerancePx;
}

bool distant_caret_position(const RECT& left, const RECT& right) {
    const auto height = (std::max)(20LL, static_cast<long long>(left.bottom) - left.top);
    const auto dx = std::llabs(static_cast<long long>(left.left) - right.left);
    const auto dy = std::llabs(static_cast<long long>(left.top) - right.top);
    return dx > (std::max)(80LL, height * 4) || dy > (std::max)(60LL, height * 2);
}

cxxime::CandidatePresentationPage project_candidate_page(const cxxime::CandidatePage& page) {
    cxxime::CandidatePresentationPage presentation;
    presentation.page_index = page.page_index;
    presentation.page_offset = page.page_offset;
    presentation.page_size = page.page_size;
    presentation.extent = page.extent;
    presentation.extent.known_count = (std::max)(
        presentation.extent.known_count,
        presentation.page_offset + static_cast<int>(page.candidates.size()));
    presentation.highlighted = page.highlighted;
    presentation.items.reserve(page.candidates.size());
    for (const cxxime::Candidate& candidate : page.candidates) {
        cxxime::CandidatePresentationItem item;
        item.text = candidate.text;
        item.hint = candidate.comment;
        item.recommended = candidate.recommended;
        presentation.items.push_back(std::move(item));
    }
    return presentation;
}

} // namespace

void CandidatePresentation::update_content(const cxxime::CandidatePresentationPage& page,
                                           const std::string& popup_preedit,
                                           std::size_t popup_preedit_cursor,
                                           std::size_t converted_prefix_bytes,
                                           std::uint64_t candidate_revision,
                                           int page_current,
                                           int page_total,
                                           std::size_t focused_preedit_start,
                                           std::size_t focused_preedit_end,
                                           bool has_syllable_boundaries) {
    advance_generation();
    if (initial_layout_wait_) {
        caret_jump_pending_ = false;
    }
    page_ = page;
    page_current_ = page_current;
    page_total_ = page_total;
    popup_preedit_ = popup_preedit;
    popup_preedit_cursor_ = (std::min)(popup_preedit_cursor, popup_preedit_.size());
    converted_prefix_bytes_ = (std::min)(converted_prefix_bytes, popup_preedit_.size());
    focused_preedit_start_ = (std::min)(focused_preedit_start, popup_preedit_.size());
    focused_preedit_end_ = (std::min)(focused_preedit_end, popup_preedit_.size());
    has_syllable_boundaries_ = has_syllable_boundaries && !popup_preedit_.empty();
    if (focused_preedit_start_ < converted_prefix_bytes_ ||
        focused_preedit_end_ < focused_preedit_start_) {
        focused_preedit_start_ = popup_preedit_.size();
        focused_preedit_end_ = popup_preedit_.size();
    } else if (focused_preedit_start_ == 0 && focused_preedit_end_ == 0 &&
               !popup_preedit_.empty()) {
        focused_preedit_start_ = popup_preedit_.size();
        focused_preedit_end_ = popup_preedit_.size();
    }
    candidate_revision_ = candidate_revision;

    if (!page_.items.empty()) {
        content_state_ = CandidateContentState::kCandidates;
    } else if (!popup_preedit_.empty()) {
        content_state_ = CandidateContentState::kPreeditOnly;
    } else {
        content_state_ = CandidateContentState::kEmpty;
        ownership_ = CandidateOwnership::kNone;
        page_current_ = 0;
        page_total_ = 0;
        reset_position_state();
    }
}

void CandidatePresentation::update_content(const cxxime::CandidatePage& page,
                                           const std::string& popup_preedit,
                                           std::size_t popup_preedit_cursor, int page_current,
                                           int page_total) {
    update_content(project_candidate_page(page), popup_preedit, popup_preedit_cursor, 0, 0,
                   page_current, page_total, popup_preedit.size(), popup_preedit.size(), false);
}

void CandidatePresentation::update_page(const cxxime::CandidatePresentationPage& page,
                                        std::uint64_t candidate_revision,
                                        int page_current,
                                        int page_total) {
    update_content(page, popup_preedit_, popup_preedit_cursor_, converted_prefix_bytes_,
                   candidate_revision, page_current, page_total, focused_preedit_start_,
                   focused_preedit_end_, has_syllable_boundaries_);
}

void CandidatePresentation::set_ownership(CandidateOwnership ownership) {
    ownership_ =
        content_state_ == CandidateContentState::kEmpty ? CandidateOwnership::kNone : ownership;
    if (ownership_ != CandidateOwnership::kExternal) {
        local_visible_candidate_count_ = 0;
    }
}

void CandidatePresentation::set_presenter(CandidatePresenter presenter) {
    if (content_state_ == CandidateContentState::kEmpty) {
        presenter = CandidatePresenter::kNone;
    }
    if (presenter_ == presenter) {
        return;
    }
    presenter_ = presenter;
    ++presentation_generation_;
    if (presentation_generation_ == 0) {
        ++presentation_generation_;
    }
    if (presenter_ != CandidatePresenter::kLocal) {
        local_visible_candidate_count_ = 0;
    }
}

void CandidatePresentation::set_local_visible_candidate_count(std::size_t count) {
    local_visible_candidate_count_ = presenter_ == CandidatePresenter::kLocal
                                         ? (std::min)(count, page_.items.size())
                                         : 0;
}

std::uint32_t CandidatePresentation::local_visible_candidate_count() const {
    return presenter_ == CandidatePresenter::kLocal
               ? static_cast<std::uint32_t>(local_visible_candidate_count_)
               : 0;
}

void CandidatePresentation::begin_waiting_for_caret(bool reposition, const RECT* stale_rect,
                                                    TimePoint now) {
    if (position_state_ != CandidatePositionState::kWaitingCaret) {
        position_state_ = CandidatePositionState::kWaitingCaret;
        reposition_wait_ = false;
        initial_layout_wait_ = false;
        has_stale_rect_ = false;
        stale_rect_ = {};
        waiting_since_ = now;
    }
    reposition_wait_ = reposition_wait_ || reposition;
    if (stale_rect) {
        stale_rect_ = *stale_rect;
        has_stale_rect_ = true;
    }
}

void CandidatePresentation::begin_waiting_for_initial_layout(const RECT& provisional_rect,
                                                             TimePoint now) {
    position_state_ = CandidatePositionState::kWaitingCaret;
    reposition_wait_ = false;
    initial_layout_wait_ = true;
    has_stale_rect_ = true;
    stale_rect_ = provisional_rect;
    waiting_since_ = now;
    has_reference_caret_ = false;
    has_displayed_caret_ = false;
    caret_jump_pending_ = false;
    caret_jump_filtered_ = false;
}

void CandidatePresentation::update_initial_layout_provisional(const RECT& provisional_rect) {
    if (!initial_layout_pending()) {
        return;
    }
    stale_rect_ = provisional_rect;
    has_stale_rect_ = true;
}

bool CandidatePresentation::pending_caret_fallback_due(TimePoint now, int delay_ms) const {
    return waiting_for_caret() &&
        caret_resolution_allowed_ && !reposition_wait_ && !has_stale_rect_ &&
        waiting_since_.time_since_epoch().count() != 0 &&
            now - waiting_since_ >= std::chrono::milliseconds(delay_ms);
}

bool CandidatePresentation::accept_provisional_caret_after_timeout(TimePoint now,
                                                                   RECT* caret_rect) {
    if (!caret_rect || !waiting_for_caret() || !caret_resolution_allowed_ ||
        !initial_layout_wait_ || !has_stale_rect_ || waiting_since_ == TimePoint{} ||
        now - waiting_since_ < kCaretSampleMaxWait) {
        return false;
    }
    *caret_rect = stale_rect_;
    reset_position_state();
    return true;
}

void CandidatePresentation::begin_composition_restart(TimePoint now) {
    position_state_ = CandidatePositionState::kWaitingCaret;
    reposition_wait_ = true;
    initial_layout_wait_ = false;
    has_stale_rect_ = false;
    stale_rect_ = {};
    waiting_since_ = now;
    composition_restart_active_ = true;
    caret_resolution_allowed_ = false;
}

bool CandidatePresentation::should_keep_waiting_for_caret(const RECT& caret_rect,
                                                          bool from_layout_change,
                                                          bool used_trusted_caret, TimePoint now,
                                                          int pending_delay_ms,
                                                          int reposition_delay_ms) {
    if (!waiting_for_caret()) {
        return false;
    }
    if (!caret_resolution_allowed_) {
        return true;
    }
    if (waiting_since_.time_since_epoch().count() == 0) {
        return false;
    }
    if (initial_layout_wait_) {
        if (used_trusted_caret) {
            return false;
        }
        if (!distant_caret_position(stale_rect_, caret_rect)) {
            caret_jump_pending_ = false;
            return now - waiting_since_ < kCaretSampleConfirmDelay;
        }
        // A layout notification can still carry a transient extent. Confirm a distant
        // first position with a later consistent sample before making it visible.
        if (caret_jump_pending_ && same_caret_position(pending_caret_, caret_rect)) {
            return now - pending_caret_since_ < kCaretSampleConfirmDelay;
        }
        pending_caret_ = caret_rect;
        pending_caret_since_ = now;
        caret_jump_pending_ = true;
        return true;
    }
    if (has_stale_rect_ && !same_caret_position(caret_rect, stale_rect_)) {
        return false;
    }

    const int delay_ms = reposition_wait_ ? reposition_delay_ms : pending_delay_ms;
    const bool deadline_reached = now - waiting_since_ >= std::chrono::milliseconds(delay_ms);
    if (deadline_reached) {
        return false;
    }
    if (!has_stale_rect_) {
        return !from_layout_change && !used_trusted_caret;
    }
    return reposition_wait_ || (!from_layout_change && !used_trusted_caret);
}

bool CandidatePresentation::complete_composition_restart(std::uint64_t generation, TimePoint now) {
    if (!generation_matches(generation) || !composition_restart_active_) {
        return false;
    }
    if (!caret_resolution_allowed_) {
        // A queued host edit cannot supply layout yet. Start the budget only once it succeeds.
        waiting_since_ = now;
        caret_resolution_allowed_ = true;
    }
    return true;
}

bool CandidatePresentation::accept_caret(std::uint64_t generation) {
    if (!caret_resolution_allowed_ || !generation_matches(generation)) {
        return false;
    }
    if (waiting_for_caret()) {
        reset_position_state();
    }
    return true;
}

RECT CandidatePresentation::display_caret(const RECT& sample, std::uint64_t sample_serial,
                                         std::uint64_t target_generation, TimePoint now) {
    caret_jump_filtered_ = false;
    if (!has_reference_caret_ || displayed_target_generation_ != target_generation) {
        displayed_caret_ = sample;
        displayed_target_generation_ = target_generation;
        has_reference_caret_ = true;
        has_displayed_caret_ = true;
        caret_jump_pending_ = false;
        return sample;
    }
    if (!distant_caret_position(displayed_caret_, sample)) {
        caret_jump_filtered_ = caret_jump_pending_;
        displayed_caret_ = sample;
        has_displayed_caret_ = true;
        caret_jump_pending_ = false;
        return sample;
    }

    if (caret_jump_pending_) {
        if (sample_serial != pending_sample_serial_ &&
            now - pending_caret_since_ >= kCaretSampleConfirmDelay) {
            displayed_caret_ = sample;
            has_displayed_caret_ = true;
            caret_jump_pending_ = false;
            return sample;
        }
    } else {
        pending_caret_since_ = now;
        pending_sample_serial_ = sample_serial;
        caret_jump_pending_ = true;
    }

    pending_caret_ = sample;
    return has_displayed_caret_ ? displayed_caret_ : sample;
}

bool CandidatePresentation::can_retain_displayed_caret(std::uint64_t target_generation) const {
    return should_show_external_window(true) && !composition_restart_active_ &&
           has_displayed_caret_ && displayed_target_generation_ == target_generation;
}

bool CandidatePresentation::caret_poll_pending() const {
    if (waiting_for_caret()) {
        return caret_resolution_allowed_ && waiting_since_ != TimePoint{};
    }
    return caret_jump_pending_;
}

bool CandidatePresentation::expire_caret_wait(TimePoint now) {
    if (!waiting_for_caret() || !caret_poll_pending()) {
        return false;
    }
    const auto budget = reposition_wait_ ? std::chrono::milliseconds(kRepositionFallbackDelayMs)
                                         : kCaretSampleMaxWait;
    if (now - waiting_since_ < budget) {
        return false;
    }
    // Keep the unresolved presentation recoverable by events, without rearming its timer.
    waiting_since_ = {};
    return true;
}

bool CandidatePresentation::accept_pending_caret_after_timeout(TimePoint now) {
    if (!caret_jump_pending_ || now - pending_caret_since_ < kCaretSampleMaxWait) {
        return false;
    }
    displayed_caret_ = pending_caret_;
    has_displayed_caret_ = true;
    caret_jump_pending_ = false;
    caret_jump_filtered_ = false;
    return true;
}

void CandidatePresentation::finish() {
    advance_generation();
    has_reference_caret_ = false;
    has_displayed_caret_ = false;
    content_state_ = CandidateContentState::kEmpty;
    ownership_ = CandidateOwnership::kNone;
    presenter_ = CandidatePresenter::kNone;
    page_ = {};
    page_current_ = 0;
    page_total_ = 0;
    popup_preedit_.clear();
    popup_preedit_cursor_ = 0;
    converted_prefix_bytes_ = 0;
    focused_preedit_start_ = 0;
    focused_preedit_end_ = 0;
    has_syllable_boundaries_ = false;
    candidate_revision_ = 0;
    reset_position_state();
}

bool CandidatePresentation::external_window_expected() const {
    return content_state_ != CandidateContentState::kEmpty &&
           ownership_ == CandidateOwnership::kExternal;
}

bool CandidatePresentation::should_show_external_window(bool composing) const {
    return composing && external_window_expected() &&
           position_state_ == CandidatePositionState::kReady;
}

void CandidatePresentation::advance_generation() {
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    ++presentation_generation_;
    if (presentation_generation_ == 0) {
        ++presentation_generation_;
    }
    local_visible_candidate_count_ = 0;
}

void CandidatePresentation::reset_position_state() {
    position_state_ = CandidatePositionState::kReady;
    composition_restart_active_ = false;
    caret_resolution_allowed_ = true;
    reposition_wait_ = false;
    initial_layout_wait_ = false;
    has_stale_rect_ = false;
    stale_rect_ = {};
    waiting_since_ = {};
    caret_jump_pending_ = false;
    caret_jump_filtered_ = false;
}

} // namespace cxxime_tsf
