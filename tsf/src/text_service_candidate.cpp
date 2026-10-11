// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdio>
#include <string>

#include <cxxime/candidate_window.h>

#include "candidate_ui_element.h"
#include "host_compatibility/host_classification_compatibility.h"
#include "reading_ui_element.h"
#include "tsf_host_classification.h"
#include "tsf_host_classification_message.h"

namespace {

bool has_caret_height(const RECT& rect) {
    return rect.bottom > rect.top;
}

} // namespace

void TextService::_show_candidate_window(const char* reason) {
    _enqueue_event_trace("ui_presentation", reason);
    _publish_ui_presentation();
    _update_state_poll_timer();
}

void TextService::_hide_external_candidate_window(const char* reason) {
    _enqueue_event_trace("ui_presentation", reason);
    _publish_ui_presentation();
    _update_state_poll_timer();
}

void TextService::_hide_candidate_window(const char* reason) {
    if (_localCandidateWindow) {
        _localCandidateWindow->reset_placement();
    }
    _localCandidatePlacementTargetGeneration = 0;
    _candidatePresentation.finish();
    _hide_candidate_projection(reason);
}

void TextService::_hide_candidate_projection(const char* reason) {
    if (_candidateUiElement) {
        _candidateUiElement->end(_threadMgr);
    }
    _hide_external_candidate_window(reason);
}

void TextService::_sync_candidate_ui_element_snapshot() {
    if (!_candidateUiElement) {
        return;
    }
    if (_candidatePresentation.has_candidates()) {
        _candidateUiElement->set_page(
            _candidatePresentation.page(), _candidatePresentation.candidate_revision(),
            _candidatePresentation.page_current(),
            _candidatePresentation.page_total());
    } else {
        _candidateUiElement->clear_page();
    }
}

void TextService::_update_reading_ui_element(ITfContext* context, const std::wstring& reading) {
    if (!_readingUiElement || !context || reading.empty()) {
        _end_reading_ui_element("hide:reading_empty");
        return;
    }

    bool was_active = _readingUiElement->is_active();
    _readingUiElement->set_reading(context, reading);
    bool external = _readingUiElement->begin(_threadMgr);
    _readingUiElement->notify_update(_threadMgr);

    if (!was_active) {
        char detail[64] = {};
        snprintf(detail, sizeof(detail), "reading external=%s len=%u",
                 external ? "true" : "false", static_cast<unsigned int>(reading.size()));
        _enqueue_event_trace("ui_element", detail);
    }
}

void TextService::_end_reading_ui_element(const char* reason) {
    if (!_readingUiElement)
        return;
    bool was_active = _readingUiElement->is_active();
    _readingUiElement->end(_threadMgr);
    if (was_active)
        _enqueue_event_trace("ui_element", reason ? reason : "hide:reading");
}

std::wstring TextService::utf8_to_wstring(const char* text) {
    if (!text || text[0] == '\0')
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], len);
    return result;
}

cxxime::CandidateUiContext TextService::_candidate_ui_context() const {
    cxxime::CandidateUiContext context;
    context.session_generation = _uiSessionGeneration;
    context.target_generation = _uiTargetGeneration;
    context.composition_generation = _candidatePresentation.generation();
    context.presentation_generation = _candidatePresentation.presentation_generation();
    context.local_visible_candidate_count =
        _candidatePresentation.local_visible_candidate_count();
    switch (_candidatePresentation.presenter()) {
    case cxxime_tsf::CandidatePresenter::kServer:
        context.presenter = cxxime::CandidateUiContext::Presenter::SERVER;
        break;
    case cxxime_tsf::CandidatePresenter::kLocal:
        context.presenter = cxxime::CandidateUiContext::Presenter::LOCAL;
        break;
    case cxxime_tsf::CandidatePresenter::kHost:
        context.presenter = cxxime::CandidateUiContext::Presenter::HOST;
        break;
    case cxxime_tsf::CandidatePresenter::kNone:
        context.presenter = cxxime::CandidateUiContext::Presenter::NONE;
        break;
    }
    return context;
}

bool TextService::_publish_candidate_ui_element() {
    if (_candidatePresentation.content_state() == cxxime_tsf::CandidateContentState::kEmpty) {
        if (_candidateUiElement) {
            _candidateUiElement->end(_threadMgr);
        }
        _candidatePresentation.set_ownership(cxxime_tsf::CandidateOwnership::kNone);
        _publish_ui_presentation();
        return false;
    }

    bool show_external = true;
    const uint32_t candidate_count =
        static_cast<uint32_t>(_candidatePresentation.page().items.size());
    if (candidate_count > 0 && _candidateUiElement) {
        const bool was_active = _candidateUiElement->is_active();
        const bool prepare_before_begin =
            !was_active && (_activateFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0;
        if (prepare_before_begin) {
            _prepare_host_candidate_compatibility();
        }
        show_external = _candidateUiElement->begin(_threadMgr, _effectiveDocumentMgr);
        _candidatePresentation.set_ownership(
            show_external ? cxxime_tsf::CandidateOwnership::kExternal
                          : cxxime_tsf::CandidateOwnership::kHost);
        _candidateUiElement->notify_update(_threadMgr);
        show_external = _candidateUiElement->wants_external_window();
        if (!was_active) {
            char detail[80] = {};
            snprintf(detail, sizeof(detail), "candidate external=%s count=%u",
                     show_external ? "true" : "false",
                     static_cast<unsigned int>(candidate_count));
            _enqueue_event_trace("ui_element", detail);
        }
    } else if (_candidateUiElement) {
        _candidateUiElement->end(_threadMgr);
    }
    _candidatePresentation.set_ownership(
        show_external ? cxxime_tsf::CandidateOwnership::kExternal
                      : cxxime_tsf::CandidateOwnership::kHost);
    _publish_ui_presentation();
    return show_external;
}

bool TextService::set_candidate_ui_element_shown(bool show) {
    if (!show) {
        _candidatePresentation.set_ownership(cxxime_tsf::CandidateOwnership::kHost);
        _hide_external_candidate_window("hide:ui_element_show");
        return true;
    }
    if (!_activated || (!_inputFocused && !_effectiveEditTarget.valid()) ||
        !_candidateUiElement || !_candidateUiElement->is_active() ||
        _candidatePresentation.content_state() == cxxime_tsf::CandidateContentState::kEmpty) {
        return false;
    }

    if (_candidatePresentation.waiting_for_caret()) {
        _candidatePresentation.set_ownership(cxxime_tsf::CandidateOwnership::kExternal);
        _publish_ui_presentation();
        _update_state_poll_timer();
        return true;
    }

    ITfContext* context = _current_edit_context_for_composition();
    RECT caret_rect = {};
    bool caret_resolved = false;
    if (context) {
        caret_resolved = _resolve_context_native_caret_rect(context, &caret_rect);
        if (!caret_resolved) {
            caret_rect = _resolve_caret_rect(context);
            caret_resolved = has_caret_height(caret_rect);
        }
    }
    _candidatePresentation.set_ownership(cxxime_tsf::CandidateOwnership::kExternal);
    if (caret_resolved) {
        set_caret_rect(caret_rect);
        _candidatePresentation.accept_caret(_candidatePresentation.generation());
        trace_caret_event("show_move", "ui_element_show", true, &caret_rect);
    } else {
        _candidatePresentation.begin_waiting_for_caret(
            false, nullptr, cxxime_tsf::CandidatePresentation::Clock::now());
        trace_caret_event("show_move", "ui_element_show", false, nullptr, E_FAIL, true);
    }

    _show_candidate_window("show:ui_element_show");
    if (context) {
        _request_candidate_position_update(context, "show:ui_element_follow");
        context->Release();
    }
    return true;
}

bool TextService::is_candidate_ui_element_shown() const {
    return _candidatePresentation.external_window_expected();
}

void TextService::_prepare_host_candidate_compatibility() {
    const cxxime_tsf::HostClassificationCompatibilitySnapshot snapshot =
        cxxime_tsf::prepare_host_classification_compatibility();
    cxxime_tsf::trace_host_classification_compatibility(snapshot);
    if (snapshot.process_matches) {
        cxxime_tsf::preflight_host_classification_compatibility(
            reinterpret_cast<HWND>(snapshot.active_hwnd));
    }
}

bool TextService::select_candidate_from_ui(UINT index, uint64_t candidate_revision) {
    if (!_candidatePresentation.has_candidates() ||
        index >= _candidatePresentation.page().items.size()) {
        return false;
    }
    const std::string& hint = _candidatePresentation.page().items[index].hint;
    if (_candidatePresentation.candidate_revision() == 0 && candidate_revision == 0 &&
        index < 9 && hint.size() == 3 && hint.front() == '/') {
        ITfContext* context = _current_edit_context_for_composition();
        if (!context) {
            return false;
        }
        BOOL eaten = FALSE;
        const bool processed =
            _ProcessKeyEvent(context, static_cast<WPARAM>('1' + index), 0, &eaten);
        context->Release();
        return processed && eaten != FALSE;
    }

    if (!_ensure_ipc_session()) {
        return false;
    }
    const cxxime::CandidateSelectionCallResult result =
        _client.select_candidate_with_revision(_sessionId, index, candidate_revision);
    if (!result.transport_success) {
        return false;
    }

    if (result.status == cxxime::IPCStatus::ERR_INVALID_SESSION) {
        _recreate_ipc_session_preserving_status();
        _hide_candidate_window("hide:select_invalid_session");
        _end_reading_ui_element("hide:select_invalid_session_reading");
        _composing = false;
        return false;
    }
    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    const bool applied = _apply_engine_response(context, result.response, &eaten);
    if (context) {
        context->Release();
    }
    return applied;
}

bool TextService::navigate_candidate_page_from_ui(bool previous) {
    cxxime::IPCResponse response = {};
    uint32_t key_code = previous ? VK_PRIOR : VK_NEXT;
    if (!_ensure_ipc_session() ||
        !_client.process_key(_sessionId, key_code, 0, response, false,
                             _candidate_ui_context())) {
        return false;
    }
    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    const bool applied = _apply_engine_response(context, response, &eaten);
    if (context) {
        context->Release();
    }
    return applied;
}

bool TextService::refresh_candidates_from_server() {
    cxxime::IPCResponse response = {};
    if (!_ensure_ipc_session() ||
        !_client.refresh_candidates(_sessionId, response, _candidate_ui_context())) {
        return false;
    }
    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    const bool applied = _apply_engine_response(context, response, &eaten);
    if (context) {
        context->Release();
    }
    return applied;
}

void TextService::abort_candidate_ui_from_tsf() {
    if (_sessionId && _client.is_connected())
        _client.clear_composition(_sessionId);
    _AbortComposition();
}

HRESULT TextService::finalize_exact_candidate_ui_from_tsf() {
    if (!_ensure_ipc_session()) {
        return E_FAIL;
    }
    cxxime::IPCResponse response = {};
    if (!_client.commit_composition(_sessionId, response)) {
        return E_FAIL;
    }

    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    const bool applied = _apply_engine_response(context, response, &eaten);
    if (context) {
        context->Release();
    }
    return applied && eaten != FALSE ? S_OK : E_FAIL;
}
