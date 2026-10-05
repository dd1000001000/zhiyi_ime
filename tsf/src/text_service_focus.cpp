// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdio>
#include <cstring>

#include <cxxime/diagnostics_config.h>

#include "candidate_ui_element.h"

namespace {

std::uintptr_t com_identity(IUnknown* object) {
    if (!object) {
        return 0;
    }

    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity))) ||
        !identity) {
        return 0;
    }

    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(identity);
    identity->Release();
    return value;
}

HWND active_view_window(ITfContext* context) {
    if (!context) {
        return nullptr;
    }

    ITfContextView* view = nullptr;
    if (FAILED(context->GetActiveView(&view)) || !view) {
        return nullptr;
    }

    HWND window = nullptr;
    const HRESULT hr = view->GetWnd(&window);
    view->Release();
    return SUCCEEDED(hr) && (!window || IsWindow(window)) ? window : nullptr;
}

bool unavailable_target_reason(const char* reason) {
    return reason && (std::strcmp(reason, "no_context") == 0 ||
                      std::strcmp(reason, "context_not_foreground") == 0);
}

} // namespace

bool TextService::_context_matches_effective_edit_target(ITfContext* context) const {
    return context && _effectiveEditTarget.valid() &&
           com_identity(context) == _effectiveEditTarget.context_identity;
}

cxxime_tsf::EffectiveEditTargetBindings
TextService::_effective_edit_target_bindings(
    const cxxime_tsf::EffectiveEditTargetSnapshot& target) const {
    cxxime_tsf::EffectiveEditTargetBindings bindings;
    bindings.has_bound_resources = _inputFocused || _effectiveDocumentMgr || _effectiveContext ||
                                   _textEditSinkContext || _textLayoutSinkContext ||
                                   (_candidateUiElement && _candidateUiElement->is_active());
    bindings.input_state_matches = !target.valid() || _inputFocused;
    bindings.target_resources_match =
        !target.valid() || (com_identity(_effectiveDocumentMgr) == target.document_identity &&
                            com_identity(_effectiveContext) == target.context_identity);
    bindings.edit_sink_matches =
        !target.valid() || (_dwTextEditSinkCookie != TF_INVALID_COOKIE &&
                            com_identity(_textEditSinkContext) == target.context_identity);
    bindings.layout_sink_matches =
        !target.valid() || (_dwTextLayoutSinkCookie != TF_INVALID_COOKIE &&
                            com_identity(_textLayoutSinkContext) == target.context_identity);

    bindings.candidate_document_matches = true;
    if (target.valid() && _candidateUiElement && _candidateUiElement->is_active()) {
        bindings.candidate_document_matches =
            com_identity(_candidateUiElement->bound_document_mgr()) == target.document_identity;
    }

    return bindings;
}

void TextService::_trace_effective_edit_target_sync(
    const char* source, cxxime_tsf::EffectiveEditTargetAction action,
    const cxxime_tsf::EffectiveEditTargetSnapshot& previous,
    const cxxime_tsf::EffectiveEditTargetSnapshot& next,
    const cxxime_tsf::EffectiveEditTargetBindings& bindings, bool succeeded) {
    const cxxime::DiagnosticsConfig diagnostics = cxxime::diagnostics_config();
    if (diagnostics.trace_mode == cxxime::DiagnosticTraceMode::kOff ||
        (diagnostics.trace_mode == cxxime::DiagnosticTraceMode::kError && succeeded)) {
        return;
    }

    char detail[640] = {};
    std::snprintf(
        detail, sizeof(detail),
        "source=%s action=%s old_doc=0x%llx old_ctx=0x%llx new_doc=0x%llx "
        "new_ctx=0x%llx view=0x%llx input=%d resources=%d edit=%d layout=%d "
        "candidate_doc=%d result=%s",
        source ? source : "unknown", cxxime_tsf::effective_edit_target_action_name(action),
        static_cast<unsigned long long>(previous.document_identity),
        static_cast<unsigned long long>(previous.context_identity),
        static_cast<unsigned long long>(next.document_identity),
        static_cast<unsigned long long>(next.context_identity),
        static_cast<unsigned long long>(next.view_window),
        bindings.input_state_matches ? 1 : 0,
        bindings.target_resources_match ? 1 : 0,
        bindings.edit_sink_matches ? 1 : 0,
        bindings.layout_sink_matches ? 1 : 0,
        bindings.candidate_document_matches ? 1 : 0,
        succeeded ? "success" : "failed");
    _enqueue_event_trace("tsf.edit_target_sync", detail, !succeeded);
}

void TextService::_release_effective_edit_target() {
    if (_effectiveContext) {
        _effectiveContext->Release();
        _effectiveContext = nullptr;
    }
    if (_effectiveDocumentMgr) {
        _effectiveDocumentMgr->Release();
        _effectiveDocumentMgr = nullptr;
    }
    _effectiveEditTarget = {};
}

void TextService::_clear_effective_edit_target(const char* source, bool target_unavailable) {
    invalidate_composition_edit_requests();
    const cxxime_tsf::EffectiveEditTargetSnapshot previous = _effectiveEditTarget;
    const cxxime_tsf::EffectiveEditTargetSnapshot unavailable;
    const cxxime_tsf::EffectiveEditTargetBindings bindings =
        _effective_edit_target_bindings(previous);
    const cxxime_tsf::EffectiveEditTargetAction action =
        cxxime_tsf::classify_effective_edit_target_change(previous, unavailable, bindings);

    _inputTargetUnavailable = target_unavailable;
    _runtimeTargetValidationFailures = 0;
    if (action == cxxime_tsf::EffectiveEditTargetAction::kUnchanged) {
        _inputFocused = false;
        ++_uiTargetGeneration;
        if (_uiTargetGeneration == 0) {
            ++_uiTargetGeneration;
        }
        _publish_ui_presentation();
        if (target_unavailable) {
            _stop_state_poll_timer();
        } else {
            _update_state_poll_timer();
        }
        return;
    }

    if (_sessionId && _client.is_connected()) {
        _client.focus_out(_sessionId);
    }
    _AbortComposition();
    _unadvise_text_edit_sink();
    _unadvise_text_layout_sink();
    _release_effective_edit_target();
    _inputFocused = false;
    ++_uiTargetGeneration;
    if (_uiTargetGeneration == 0) {
        ++_uiTargetGeneration;
    }
    _refresh_ui_presentation("hide:edit_target_clear");
    if (target_unavailable) {
        _stop_state_poll_timer();
    } else {
        _update_state_poll_timer();
    }
    _trace_effective_edit_target_sync(source, action, previous, unavailable, bindings, true);
}

bool TextService::_synchronize_effective_edit_target(ITfContext* event_context,
                                                     ITfDocumentMgr* event_document_mgr,
                                                     const char* source) {
    ITfDocumentMgr* document_mgr = nullptr;
    ITfContext* context = nullptr;

    if (event_context) {
        context = event_context;
        context->AddRef();
        if (FAILED(context->GetDocumentMgr(&document_mgr)) || !document_mgr) {
            if (event_document_mgr) {
                document_mgr = event_document_mgr;
                document_mgr->AddRef();
            }
        }
    } else {
         if (event_document_mgr) {
            document_mgr = event_document_mgr;
            document_mgr->AddRef();
         } else if (_threadMgr) {
            _threadMgr->GetFocus(&document_mgr);
         }
        if (document_mgr) {
            document_mgr->GetTop(&context);
        }
    }

    if (!document_mgr || !context) {
        if (context) {
            context->Release();
        }
        if (document_mgr) {
            document_mgr->Release();
        }
        _clear_effective_edit_target(source);
        return false;
    }

    const char* block_reason = _input_context_block_reason(context);
    if (block_reason) {
        context->Release();
        document_mgr->Release();
        _clear_effective_edit_target(source, unavailable_target_reason(block_reason));
        return false;
    }

    cxxime_tsf::EffectiveEditTargetSnapshot next;
    next.document_identity = com_identity(document_mgr);
    next.context_identity = com_identity(context);
    HWND view_window = active_view_window(context);
    next.view_window = reinterpret_cast<std::uintptr_t>(view_window);
    next.editable = next.document_identity != 0 && next.context_identity != 0;
    if (!next.valid()) {
        context->Release();
        document_mgr->Release();
        _clear_effective_edit_target(source);
        return false;
    }

    const cxxime_tsf::EffectiveEditTargetSnapshot previous = _effectiveEditTarget;
    const cxxime_tsf::EffectiveEditTargetBindings bindings =
        _effective_edit_target_bindings(next);
    const cxxime_tsf::EffectiveEditTargetAction action =
        cxxime_tsf::classify_effective_edit_target_change(previous, next, bindings);
    const bool context_changed = previous.context_identity != next.context_identity;
    const bool document_changed = previous.document_identity != next.document_identity;

    if (action == cxxime_tsf::EffectiveEditTargetAction::kRebind && previous.valid() &&
        (context_changed || document_changed)) {
        if (_composing && _sessionId && _client.is_connected()) {
            _client.focus_out(_sessionId);
        }
        _AbortComposition();
        _unadvise_text_edit_sink();
        _unadvise_text_layout_sink();
        _release_effective_edit_target();
    }

    if (action == cxxime_tsf::EffectiveEditTargetAction::kRebind ||
        !bindings.target_resources_match) {
        if (_effectiveContext != context || _effectiveDocumentMgr != document_mgr) {
            _release_effective_edit_target();
            document_mgr->AddRef();
            _effectiveDocumentMgr = document_mgr;
            context->AddRef();
            _effectiveContext = context;
        }
        _effectiveEditTarget = next;
    }

    bool repaired = true;
    if ((action == cxxime_tsf::EffectiveEditTargetAction::kRebind &&
         (context_changed || document_changed)) ||
        !bindings.edit_sink_matches) {
        repaired = _bind_text_edit_sink(context) && repaired;
    }
    if ((action == cxxime_tsf::EffectiveEditTargetAction::kRebind &&
         (context_changed || document_changed)) ||
        !bindings.layout_sink_matches) {
        repaired = _bind_text_layout_sink(context) && repaired;
    }

    const bool candidate_was_active = _candidateUiElement && _candidateUiElement->is_active();
    if (candidate_was_active && !bindings.candidate_document_matches) {
        _candidateUiElement->end(_threadMgr);
    }

    if (candidate_was_active && !bindings.candidate_document_matches &&
        (_composing || _candidatePresentation.waiting_for_caret()) &&
        _candidatePresentation.has_candidates()) {
        _sync_candidate_ui_element_snapshot();
        _publish_candidate_ui_element();
    }

    _inputFocused = true;
    _inputTargetUnavailable = false;
    _runtimeTargetValidationFailures = 0;
    if (action == cxxime_tsf::EffectiveEditTargetAction::kRebind || context_changed ||
        document_changed) {
        ++_uiTargetGeneration;
        if (_uiTargetGeneration == 0) {
            ++_uiTargetGeneration;
        }
    }
    _update_state_poll_timer();
    _publish_ui_presentation();

    if (action != cxxime_tsf::EffectiveEditTargetAction::kUnchanged) {
        const cxxime_tsf::EffectiveEditTargetBindings repaired_bindings =
            _effective_edit_target_bindings(next);
        repaired = repaired && repaired_bindings.healthy();
        _trace_effective_edit_target_sync(source, action, previous, next, bindings, repaired);
    }

    context->Release();
    document_mgr->Release();
    return true;
}

bool TextService::_synchronize_effective_edit_target_from_thread_mgr(const char* source) {
    return _synchronize_effective_edit_target(nullptr, nullptr, source);
}

STDMETHODIMP TextService::OnSetThreadFocus() {
    _note_focus_change();
    if (!_activated) {
        return S_OK;
    }

    if (_synchronize_effective_edit_target_from_thread_mgr("thread_focus")) {
        _refresh_caps_lock_on_focus("thread_focus");
        _schedule_caps_lock_refresh();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKillThreadFocus() {
    if (!_activated) {
        return S_OK;
    }

    _clear_effective_edit_target("thread_focus_lost");
    return S_OK;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) {
    UNREFERENCED_PARAMETER(pDocMgr);
    return S_OK;
}

STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) {
    if (_activated && pDocMgr && com_identity(pDocMgr) == _effectiveEditTarget.document_identity) {
        _clear_effective_edit_target("document_uninit");
    }
    return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                                     ITfDocumentMgr* pDocMgrPrevFocus) {
    UNREFERENCED_PARAMETER(pDocMgrPrevFocus);
    _note_focus_change();  // the IMM layer may restore this box's own mode now
    if (!_activated) {
        return S_OK;
    }

    if (!_synchronize_effective_edit_target(nullptr, pDocMgrFocus, "document_focus")) {
        return S_OK;
    }

    cxxime::IPCResponse response = {};
    if (_ensure_ipc_session() && _client.get_status(_sessionId, response) &&
        response.status == cxxime::IPCStatus::OK) {
        _sync_ime_status(response.ime_status);
    }
    _refresh_caps_lock_on_focus("document_focus");
    _schedule_caps_lock_refresh();
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext* pic) {
    if (_activated && _synchronize_effective_edit_target(pic, nullptr, "push_context")) {
        _refresh_caps_lock_on_focus("push_context");
        _schedule_caps_lock_refresh();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnPopContext(ITfContext* pic) {
    UNREFERENCED_PARAMETER(pic);
    if (_activated && _synchronize_effective_edit_target_from_thread_mgr("pop_context")) {
        _refresh_caps_lock_on_focus("pop_context");
        _schedule_caps_lock_refresh();
    }
    return S_OK;
}
