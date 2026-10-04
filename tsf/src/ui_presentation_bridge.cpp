// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <algorithm>
#include <cstring>

#include <cxxime/candidate_window.h>
#include <cxxime/ime_menu.h>

#include "config_coordinator.h"

namespace {

constexpr std::size_t kMaxPendingUiCommands = 64;

bool has_flag(const cxxime::UiPresentationSnapshot& snapshot, cxxime::UiSnapshotFlag flag) {
    return (snapshot.flags & cxxime::ui_snapshot_flag(flag)) != 0;
}

bool host_suppresses_status_window(bool ui_element_only,
                                   bool immersive_mode,
                                   std::uint64_t target_window,
                                   cxxime::UiOwnership ownership) {
    if (ui_element_only || ownership == cxxime::UiOwnership::kHost) {
        return true;
    }
    if (!immersive_mode) {
        return false;
    }
    const HWND window = reinterpret_cast<HWND>(target_window);
    const HWND root = window ? GetAncestor(window, GA_ROOT) : nullptr;
    return !root || root == window;
}

cxxime::UiOwnership ui_ownership(cxxime_tsf::CandidateOwnership ownership) {
    switch (ownership) {
    case cxxime_tsf::CandidateOwnership::kExternal:
        return cxxime::UiOwnership::kExternal;
    case cxxime_tsf::CandidateOwnership::kHost:
        return cxxime::UiOwnership::kHost;
    case cxxime_tsf::CandidateOwnership::kNone:
    default:
        return cxxime::UiOwnership::kNone;
    }
}

void copy_packet_text(char* destination, std::size_t capacity, std::uint32_t* length,
                      const std::string& source) {
    const std::size_t copied = (std::min)(capacity, source.size());
    if (copied > 0) {
        std::memcpy(destination, source.data(), copied);
    }
    *length = static_cast<std::uint32_t>(copied);
}

bool current_process_is_elevated() {
    static const bool elevated = []() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }

        TOKEN_ELEVATION elevation = {};
        DWORD returned_size = 0;
        const bool result = GetTokenInformation(token, TokenElevation, &elevation,
                                                sizeof(elevation), &returned_size) != FALSE &&
            elevation.TokenIsElevated != 0;
        CloseHandle(token);
        return result;
    }();
    return elevated;
}

} // namespace

bool TextService::_present_local_candidate_window(const cxxime::CandidatePresentationPage& page,
                                                  int page_current, int page_total,
                                                  const std::string& preedit,
                                                  std::size_t preedit_cursor,
                                                  const RECT& caret) {
    if (_localCandidateWindow &&
        _localCandidatePlacementTargetGeneration != _uiTargetGeneration) {
        _localCandidateWindow->reset_placement();
    }
    _localCandidatePlacementTargetGeneration = _uiTargetGeneration;
    HWND owner = reinterpret_cast<HWND>(_effectiveEditTarget.view_window);
    if (!owner || !IsWindow(owner)) {
        owner = GetFocus();
    }
    if (!owner || !IsWindow(owner)) {
        return false;
    }

    if (!_localCandidateWindow) {
        _localCandidateWindow = std::make_unique<cxxime::CandidateWindow>();
        if (!_localCandidateWindow->create(owner, _config) ||
            !_localCandidateWindow->owner_matches(owner)) {
            _localCandidateWindow.reset();
            return false;
        }
        _localCandidateWindow->set_candidate_selection_callback(
            [this](std::size_t index) {
                select_candidate_from_ui(
                    static_cast<UINT>(index), _candidatePresentation.candidate_revision());
            });
        _localCandidateWindow->set_page_callback(
            [this](cxxime::CandidatePageDirection direction) {
                navigate_candidate_page_from_ui(
                    direction == cxxime::CandidatePageDirection::Previous);
            });
        _localCandidateWindow->set_layout_changed_callback([this]() {
            if (!_localCandidateWindow || !_localCandidateWindow->is_visible() ||
                _candidatePresentation.presenter() !=
                    cxxime_tsf::CandidatePresenter::kLocal) {
                return;
            }
            _candidatePresentation.set_local_visible_candidate_count(
                static_cast<std::size_t>(
                    _localCandidateWindow->visible_candidate_count()));
        });
    } else if (!_localCandidateWindow->ensure_created(owner)) {
        return false;
    }

    _localCandidateWindow->set_page_info(page_current, page_total);
    _localCandidateWindow->set_preedit(
        preedit, preedit_cursor, _candidatePresentation.converted_prefix_bytes(),
        _candidatePresentation.focused_preedit_start(),
        _candidatePresentation.focused_preedit_end(),
        _candidatePresentation.has_syllable_boundaries());
    _localCandidateWindow->move_to_caret(caret);
    _localCandidateWindow->update(page);
    _localCandidateWindow->show();
    if (!_localCandidateWindow->is_visible()) {
        return false;
    }
    _candidatePresentation.set_presenter(cxxime_tsf::CandidatePresenter::kLocal);
    _candidatePresentation.set_local_visible_candidate_count(
        static_cast<std::size_t>(_localCandidateWindow->visible_candidate_count()));
    return true;
}

void TextService::_hide_local_candidate_window() {
    _candidatePresentation.set_local_visible_candidate_count(0);
    if (_localCandidateWindow) {
        _localCandidateWindow->hide();
    }
}

bool TextService::_start_ui_presentation_channel() {
    if (_uiChannel.is_running()) {
        return true;
    }
    if (!_configWindow) {
        return false;
    }
    return _uiChannel.start(
        [this](const cxxime::UiCommand& command) { _queue_ui_command(command); });
}

void TextService::_stop_ui_presentation_channel() {
    _hide_local_candidate_window();
    _stop_input_indicator_refresh_retry();
    _uiChannel.stop();
    std::lock_guard<std::mutex> lock(_uiCommandMutex);
    _uiCommands.clear();
    _pendingInputIndicatorRefreshCommand.reset();
}

void TextService::_publish_ui_presentation() {
    if (!_uiChannel.is_running() || _sessionId == 0 || _uiSessionGeneration == 0) {
        return;
    }
    if (_uiPresentationBatchDepth != 0) {
        _uiPresentationPublishPending = true;
        return;
    }

    cxxime::UiPresentationSnapshot snapshot;
    snapshot.session_id = _sessionId;
    snapshot.session_generation = _uiSessionGeneration;
    snapshot.target_generation = _uiTargetGeneration;
    snapshot.composition_generation = _candidatePresentation.generation();
    snapshot.ownership = ui_ownership(_candidatePresentation.ownership());
    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        snapshot.ime_status = _lastImeStatus;
    }

    if (_composing) {
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kComposing);
    }
    if (is_immersive_mode()) {
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kImmersiveMode);
    }
    snapshot.target_window = _effectiveEditTarget.view_window;
    // A top-level immersive target includes surfaces such as Start/SearchUI.
    // Framed immersive applications retain CxxIME's normal status presentation.
    const bool host_suppresses_status = host_suppresses_status_window(
        (_activateFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0, is_immersive_mode(),
        snapshot.target_window, snapshot.ownership);
    const bool status_visible =
        _activated && _inputFocused && _effectiveEditTarget.valid() &&
        _has_synced_ime_status() && _config.status_window.enable &&
        !host_suppresses_status;
    if (status_visible) {
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kStatusVisible);
    }

    if (cxxime_tsf::is_valid_caret_rect(_caretRect)) {
        snapshot.caret = _caretRect;
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret);
    }

    const std::string& preedit = _candidatePresentation.popup_preedit();
    if (!preedit.empty()) {
        copy_packet_text(snapshot.preedit, sizeof(snapshot.preedit), &snapshot.preedit_length,
                         preedit);
        snapshot.preedit_cursor = static_cast<std::uint32_t>(
            (std::min)(_candidatePresentation.popup_preedit_cursor(), preedit.size()));
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasPreedit);
    }

    const cxxime::CandidatePresentationPage& page = _candidatePresentation.page();
    snapshot.candidate_page.count = static_cast<std::uint32_t>(
        (std::min)(page.items.size(), static_cast<std::size_t>(cxxime::kCandidateCapacity)));
    snapshot.candidate_page.offset = static_cast<std::uint32_t>((std::max)(0, page.page_offset));
    const int returned_end = page.page_offset + static_cast<int>(snapshot.candidate_page.count);
    snapshot.candidate_known_count = static_cast<std::uint32_t>(
        (std::max)(page.extent.known_count, returned_end));
    snapshot.candidate_extent_state = page.extent.state;
    snapshot.candidate_extent_complete = page.extent.complete ? 1u : 0u;
    snapshot.candidate_page.total = static_cast<std::uint32_t>((std::max)(
        0, cxxime::legacy_candidate_total(page.extent, returned_end)));
    snapshot.candidate_page.highlighted =
        snapshot.candidate_page.count == 0
            ? 0
            : static_cast<std::uint32_t>(
                (std::max)(0, (std::min)(page.highlighted,
                                         static_cast<int>(snapshot.candidate_page.count - 1))));
    snapshot.candidate_page.page_current =
        static_cast<std::uint32_t>((std::max)(1, _candidatePresentation.page_current()));
    snapshot.candidate_page.page_total = static_cast<std::uint32_t>(
        (std::max)(snapshot.candidate_page.page_current,
                   static_cast<std::uint32_t>((std::max)(1, _candidatePresentation.page_total()))));
    for (std::uint32_t index = 0; index < snapshot.candidate_page.count; ++index) {
        const cxxime::CandidatePresentationItem& candidate = page.items[index];
        cxxime::UiCandidate& target = snapshot.candidate_page.candidates[index];
        if (candidate.recommended) snapshot.candidate_recommended_mask |= 1u << index;
        copy_packet_text(target.text, sizeof(target.text), &target.text_length, candidate.text);
        // Short hints (Wubi codes) fit the hint field; the full text goes to the comments.
        if (candidate.hint.size() <= sizeof(target.hint)) {
            copy_packet_text(target.hint, sizeof(target.hint), &target.hint_length, candidate.hint);
        }
        cxxime::UiCandidateComment& comment = snapshot.candidate_comments[index];
        std::string full = candidate.hint;
        if (full.size() > sizeof(comment.text)) {
            std::size_t end = sizeof(comment.text);
            while (end > 0 && (static_cast<unsigned char>(full[end]) & 0xc0) == 0x80) --end;
            full.resize(end);
        }
        copy_packet_text(comment.text, sizeof(comment.text), &comment.length, full);
    }
    if (snapshot.candidate_page.count != 0) {
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates);
    }
    snapshot.candidate_revision = _candidatePresentation.candidate_revision();
    snapshot.converted_prefix_bytes = static_cast<std::uint32_t>(
        _candidatePresentation.converted_prefix_bytes());
    snapshot.focused_preedit_start_bytes = static_cast<std::uint32_t>(
        _candidatePresentation.focused_preedit_start());
    snapshot.focused_preedit_end_bytes = static_cast<std::uint32_t>(
        _candidatePresentation.focused_preedit_end());
    snapshot.preedit_presentation_flags = _candidatePresentation.has_syllable_boundaries()
            ? cxxime::preedit_presentation_flag(
                cxxime::PreeditPresentationFlag::SyllableBoundaries)
            : 0;

    bool candidate_visible = snapshot.ownership == cxxime::UiOwnership::kExternal &&
                             _candidatePresentation.should_show_external_window(_composing) &&
                             has_flag(snapshot, cxxime::UiSnapshotFlag::kHasCaret);
    if (candidate_visible) {
        const bool was_pending = _candidatePresentation.caret_jump_pending();
        snapshot.caret = _candidatePresentation.display_caret(
            _caretRect, _caretSampleSerial, _uiTargetGeneration,
            cxxime_tsf::CandidatePresentation::Clock::now());
        const bool jump_pending = _candidatePresentation.caret_jump_pending();
        if (!was_pending && jump_pending) {
            trace_caret_event("jump_hold", "stabilizer", true, &_caretRect, S_FALSE, true);
        } else if (was_pending && !jump_pending) {
            if (_candidatePresentation.caret_jump_filtered()) {
                trace_caret_event("jump_filtered", "stabilizer", true, &snapshot.caret, S_OK,
                                true);
            } else {
                trace_caret_event("jump_confirm", "stabilizer", true, &snapshot.caret, S_OK,
                                true);
            }
        }
        if (was_pending != jump_pending) {
            _update_state_poll_timer();
        }
        candidate_visible = _candidatePresentation.caret_ready_to_show();
        if (candidate_visible) {
            snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible);
        }
    }
    const bool local_candidate_preferred =
        is_immersive_mode() || current_process_is_elevated();
    const bool local_candidate_visible =
        candidate_visible && local_candidate_preferred &&
        _present_local_candidate_window(
            page, static_cast<int>(snapshot.candidate_page.page_current),
            static_cast<int>(snapshot.candidate_page.page_total),
            _candidatePresentation.popup_preedit(),
            _candidatePresentation.popup_preedit_cursor(), snapshot.caret);
    if (local_candidate_visible) {
        snapshot.flags |=
            cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate);
        snapshot.local_candidate_window =
            reinterpret_cast<std::uint64_t>(_localCandidateWindow->native_handle());
    } else if (snapshot.ownership == cxxime::UiOwnership::kHost) {
        _hide_local_candidate_window();
        _candidatePresentation.set_presenter(cxxime_tsf::CandidatePresenter::kHost);
    } else if (candidate_visible) {
        _hide_local_candidate_window();
        _candidatePresentation.set_presenter(cxxime_tsf::CandidatePresenter::kServer);
    } else {
        _hide_local_candidate_window();
        _candidatePresentation.set_presenter(cxxime_tsf::CandidatePresenter::kNone);
    }
    snapshot.presentation_generation = _candidatePresentation.presentation_generation();
    const cxxime::DiagnosticTraceMode trace_mode = _config.diagnostics.trace_mode;
    if (trace_mode == cxxime::DiagnosticTraceMode::kNormal ||
        trace_mode == cxxime::DiagnosticTraceMode::kVerbose) {
        _enqueue_ui_presentation_trace(snapshot);
    }
    _uiChannel.publish_latest(snapshot);
}

void TextService::_publish_ui_session_ended() {
    _hide_local_candidate_window();
    if (_localCandidateWindow) {
        _localCandidateWindow->reset_placement();
    }
    _localCandidatePlacementTargetGeneration = 0;
    if (!_uiChannel.is_running() || _sessionId == 0 || _uiSessionGeneration == 0) {
        return;
    }
    cxxime::UiPresentationSnapshot snapshot;
    snapshot.session_id = _sessionId;
    snapshot.session_generation = _uiSessionGeneration;
    snapshot.target_generation = _uiTargetGeneration;
    snapshot.composition_generation = _candidatePresentation.generation();
    snapshot.presentation_generation = _candidatePresentation.presentation_generation();
    snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kSessionEnded);
    _uiChannel.publish_latest(snapshot);
}

void TextService::_queue_ui_command(const cxxime::UiCommand& command) {
    {
        std::lock_guard<std::mutex> lock(_uiCommandMutex);
        if (command.type == cxxime::UiCommandType::kRefreshInputIndicator) {
            _pendingInputIndicatorRefreshCommand = command;
        } else {
            if (_uiCommands.size() >= kMaxPendingUiCommands) {
                return;
            }
            _uiCommands.push_back(command);
        }
    }
    if (_configWindow) {
        PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_UI_COMMAND, 0, 0);
    }
}

bool TextService::_is_current_ui_command(const cxxime::UiCommand& command) const {
    if (!_activated || command.session_id != _sessionId ||
        command.session_generation != _uiSessionGeneration) {
        return false;
    }
    if (command.type == cxxime::UiCommandType::kRefreshInputIndicator) {
        return command.target_generation == 0 && command.composition_generation == 0 &&
               command.presentation_generation == 0;
    }
    if (command.target_generation != _uiTargetGeneration) {
        return false;
    }
    switch (command.type) {
    case cxxime::UiCommandType::kSelectCandidate:
    case cxxime::UiCommandType::kPagePrevious:
    case cxxime::UiCommandType::kPageNext:
        return command.composition_generation == _candidatePresentation.generation() &&
               command.presentation_generation ==
                   _candidatePresentation.presentation_generation() &&
               (command.type != cxxime::UiCommandType::kSelectCandidate ||
                command.candidate_revision == _candidatePresentation.candidate_revision()) &&
               _candidatePresentation.presenter() ==
                   cxxime_tsf::CandidatePresenter::kServer;
    case cxxime::UiCommandType::kCommitComposition:
    case cxxime::UiCommandType::kCancelComposition:
        return command.composition_generation == _candidatePresentation.generation() &&
               command.presentation_generation ==
                   _candidatePresentation.presentation_generation();
    default:
        return true;
    }
}

void TextService::_handle_ui_command(const cxxime::UiCommand& command) {
    if (!_is_current_ui_command(command)) {
        return;
    }

    cxxime::IPCResponse response = {};
    switch (command.type) {
    case cxxime::UiCommandType::kSelectCandidate:
        select_candidate_from_ui(command.candidate_index, command.candidate_revision);
        break;
    case cxxime::UiCommandType::kPagePrevious:
        navigate_candidate_page_from_ui(true);
        break;
    case cxxime::UiCommandType::kPageNext:
        navigate_candidate_page_from_ui(false);
        break;
    case cxxime::UiCommandType::kToggleChinese:
        if (_ensure_ipc_session() && _client.toggle_chinese(_sessionId, response) &&
            response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
        break;
    case cxxime::UiCommandType::kToggleShape:
        if (_ensure_ipc_session() && _client.toggle_shape(_sessionId, response) &&
            response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
        break;
    case cxxime::UiCommandType::kTogglePunct:
        if (_ensure_ipc_session() && _client.toggle_punct(_sessionId, response) &&
            response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
        break;
    case cxxime::UiCommandType::kSwitchInputMode:
        if (_ensure_ipc_session() &&
            _client.switch_input_mode(_sessionId, static_cast<cxxime::InputMode>(command.value),
                                      response) &&
            response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
        break;
    case cxxime::UiCommandType::kToggleStatusWindow:
        cxxime_tsf::set_status_window_enabled(!_config.status_window.enable);
        break;
    case cxxime::UiCommandType::kOpenSettings:
        _handle_ime_menu_command(cxxime::ImeMenuCommand::kSettings);
        break;
    case cxxime::UiCommandType::kOpenDictionary:
        _handle_ime_menu_command(cxxime::ImeMenuCommand::kDictionary);
        break;
    case cxxime::UiCommandType::kOpenAbout:
        _handle_ime_menu_command(cxxime::ImeMenuCommand::kAbout);
        break;
    case cxxime::UiCommandType::kMenuCommand:
        if (cxxime::find_ime_menu_item(command.value)) {
            _handle_ime_menu_command(static_cast<cxxime::ImeMenuCommand>(command.value));
        }
        break;
    case cxxime::UiCommandType::kRefreshInputIndicator:
        _inputIndicatorRefreshRetryCount = 0;
        _stop_input_indicator_refresh_retry();
        _refresh_input_indicator();
        return;
    case cxxime::UiCommandType::kCommitComposition:
        finalize_exact_candidate_ui_from_tsf();
        break;
    case cxxime::UiCommandType::kCancelComposition:
        _AbortComposition();
        break;
    case cxxime::UiCommandType::kNone:
        break;
    }
    _publish_ui_presentation();
}

void TextService::_drain_ui_commands() {
    std::deque<cxxime::UiCommand> commands;
    std::optional<cxxime::UiCommand> indicator_refresh;
    {
        std::lock_guard<std::mutex> lock(_uiCommandMutex);
        commands.swap(_uiCommands);
        indicator_refresh.swap(_pendingInputIndicatorRefreshCommand);
    }
    if (indicator_refresh) {
        _handle_ui_command(*indicator_refresh);
    }
    for (const cxxime::UiCommand& command : commands) {
        _handle_ui_command(command);
    }
}
