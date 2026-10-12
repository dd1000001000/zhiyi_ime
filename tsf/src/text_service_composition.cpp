// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <climits>
#include <new>
#include <vector>

#include <cxxime/logging.h>

#include "composition_termination.h"
#include "edit_session.h"
#include "preedit_mode.h"
#include "tsf_composition.h"

namespace {

bool edit_request_can_defer(HRESULT request, HRESULT session) {
    return request == TF_E_LOCKED || request == TF_E_SYNCHRONOUS ||
           (SUCCEEDED(request) && session == TF_E_SYNCHRONOUS);
}

HRESULT replace_composition_text_if_unchanged(ITfRange* range, TfEditCookie edit_cookie,
                                              const std::wstring& expected,
                                              const std::wstring& replacement) {
    if (!range || expected.empty() || expected == replacement) {
        return E_INVALIDARG;
    }
    if (expected.size() >= ULONG_MAX || replacement.size() > LONG_MAX) {
        return E_INVALIDARG;
    }
    HRESULT operation_result = S_OK;
    const cxxime_tsf::HostTerminationTextResult result =
        cxxime_tsf::normalize_host_termination_text(
            expected, replacement,
            [&](std::wstring* current) {
                std::vector<wchar_t> buffer(expected.size() + 1, L'\0');
                ULONG fetched = 0;
                operation_result = range->GetText(edit_cookie, 0, buffer.data(),
                                                  static_cast<ULONG>(buffer.size()), &fetched);
                if (FAILED(operation_result)) {
                    return false;
                }
                current->assign(buffer.data(), fetched);
                return true;
            },
            [&](const std::wstring& text) {
                operation_result =
                    range->SetText(edit_cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
                return SUCCEEDED(operation_result);
            });
    if (result == cxxime_tsf::HostTerminationTextResult::kUnchanged) {
        return S_FALSE;
    }
    return result == cxxime_tsf::HostTerminationTextResult::kReplaced ? S_OK : operation_result;
}

} // namespace

void TextService::set_composition_context(ITfContext* context) {
    if (_compositionContext == context) {
        return;
    }

    if (_compositionContext) {
        _compositionContext->Release();
    }

    _compositionContext = context;
    if (_compositionContext) {
        _compositionContext->AddRef();
    }
}

ITfContext* TextService::_current_edit_context_for_composition() const {
    if (_compositionContext) {
        _compositionContext->AddRef();
        return _compositionContext;
    }

    if (!_threadMgr) {
        return nullptr;
    }

    ITfDocumentMgr* doc_mgr = nullptr;
    if (FAILED(_threadMgr->GetFocus(&doc_mgr)) || !doc_mgr) {
        return nullptr;
    }

    ITfContext* context = nullptr;
    doc_mgr->GetTop(&context);
    doc_mgr->Release();
    return context;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite,
                                                  ITfComposition* pComposition) {
    if (_composition && pComposition && _composition != pComposition) {
        _enqueue_event_trace("composition_terminated", "source=stale action=ignore");
        return S_OK;
    }

    const bool host_terminated = _composition != nullptr;
    bool clear_succeeded = false;
    const bool normalization_requested =
        host_terminated &&
        (_emptyCompositionPlaceholderActive || _hostTerminationCompositionText.has_value());
    HRESULT normalization_result = normalization_requested ? E_POINTER : S_FALSE;
    if (host_terminated) {
        invalidate_composition_edit_requests();
        if (normalization_requested && pComposition) {
            ITfRange* range = nullptr;
            if (SUCCEEDED(pComposition->GetRange(&range)) && range) {
                if (_emptyCompositionPlaceholderActive) {
                    normalization_result =
                        replace_composition_text_if_unchanged(range, ecWrite, L" ", L"");
                } else {
                    normalization_result = replace_composition_text_if_unchanged(
                        range, ecWrite, _lastInlineCompositionText,
                        *_hostTerminationCompositionText);
                }
                range->Release();
            }
        }
        if (_sessionId) {
            clear_succeeded = _client.clear_composition(_sessionId);
        }
        _hide_candidate_window("hide:composition_terminated");
        _reset_trace_composition("host_terminated");
    }

    char detail[160] = {};
    snprintf(detail, sizeof(detail),
             "source=%s action=%s clear_succeeded=%d normalize_requested=%d normalize=0x%08lx",
             host_terminated ? "host" : "self", host_terminated ? "cancel" : "cleanup",
             clear_succeeded ? 1 : 0, normalization_requested ? 1 : 0,
             static_cast<unsigned long>(normalization_result));
    _enqueue_event_trace("composition_terminated", detail,
                         (host_terminated && !clear_succeeded) ||
                             (normalization_requested && FAILED(normalization_result)));

    _composing = false;
    _emptyCompositionPlaceholderActive = false;
    _voiceComposition = false;  // the program kept the voice text
    _voiceText.clear();
    clear_applied_inline_composition_text();
    _end_reading_ui_element("hide:composition_terminated_reading");
    if (_composition) {
        _composition->Release();
        _composition = nullptr;
    }
    set_composition_context(nullptr);
    return S_OK;
}

HRESULT TextService::insert_text(const std::wstring& text, bool sync,
                                 uint64_t* request_generation) {
    const uint64_t generation = begin_composition_edit_request();
    if (request_generation) {
        *request_generation = generation;
    }
    if (!_threadMgr || text.empty()) {
        return E_FAIL;
    }

    ITfDocumentMgr* document_mgr = nullptr;
    if (FAILED(_threadMgr->GetFocus(&document_mgr)) || !document_mgr) {
        return E_FAIL;
    }

    ITfContext* context = nullptr;
    if (FAILED(document_mgr->GetTop(&context)) || !context) {
        document_mgr->Release();
        return E_FAIL;
    }

    EditSession* edit_session = new (std::nothrow) EditSession(this, context);
    if (!edit_session) {
        context->Release();
        document_mgr->Release();
        return E_OUTOFMEMORY;
    }

    edit_session->set_action(EditSession::Action::INSERT_TEXT, text);
    edit_session->set_composition_edit_request(generation, _effectiveEditTarget.context_identity);

    HRESULT edit_hr = E_FAIL;
    const DWORD flags =
        TF_ES_READWRITE | ordered_composition_edit_mode(sync ? TF_ES_SYNC : TF_ES_ASYNC);
    const HRESULT request_hr =
        context->RequestEditSession(_clientId, edit_session, flags, &edit_hr);
    if (sync) {
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "insert sync=1 request=0x%08lx edit=0x%08lx len=%u",
                 static_cast<unsigned long>(request_hr), static_cast<unsigned long>(edit_hr),
                 static_cast<unsigned int>(text.length()));
        _enqueue_event_trace("composition_commit", detail, FAILED(request_hr) || FAILED(edit_hr));
    }

    const HRESULT action_hr = edit_session->action_result();
    edit_session->Release();
    context->Release();
    document_mgr->Release();
    if (FAILED(request_hr) || FAILED(edit_hr)) {
        return FAILED(request_hr) ? request_hr : edit_hr;
    }
    return action_hr == E_PENDING ? edit_hr : action_hr;
}

HRESULT TextService::_commit_text(ITfContext* context,
                                   const std::wstring& text,
                                   bool sync,
                                   uint64_t* request_generation) {
    if (!context) {
        return insert_text(text, sync, request_generation);
    }

    const uint64_t generation = begin_composition_edit_request();
    if (request_generation) {
        *request_generation = generation;
    }
    EditSession* edit_session = new (std::nothrow) EditSession(this, context);
    if (!edit_session) {
        return E_OUTOFMEMORY;
    }

    edit_session->set_action(EditSession::Action::COMMIT_COMPOSITION, text);
    edit_session->set_composition_edit_request(generation, _effectiveEditTarget.context_identity);

    HRESULT edit_hr = E_FAIL;
    const DWORD mode = ordered_composition_edit_mode(sync ? TF_ES_SYNC : TF_ES_ASYNCDONTCARE);
    const DWORD flags = TF_ES_READWRITE | mode;
    HRESULT request_hr =
        context->RequestEditSession(_clientId, edit_session, flags, &edit_hr);
    const HRESULT action_hr = edit_session->action_result();
    if (mode == TF_ES_SYNC && action_hr == E_PENDING &&
        edit_request_can_defer(request_hr, edit_hr)) {
        char detail[128] = {};
        snprintf(detail, sizeof(detail),
                 "commit sync_fallback request=0x%08lx edit=0x%08lx action=0x%08lx len=%u",
                 static_cast<unsigned long>(request_hr), static_cast<unsigned long>(edit_hr),
                 static_cast<unsigned long>(action_hr),
                 static_cast<unsigned int>(text.length()));
        _enqueue_event_trace("composition_commit", detail, true);
        edit_hr = E_FAIL;
        request_hr = context->RequestEditSession(
            _clientId, edit_session, TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &edit_hr);
    } else if (sync) {
        char detail[128] = {};
        snprintf(detail, sizeof(detail),
                 "commit sync=1 request=0x%08lx edit=0x%08lx action=0x%08lx len=%u",
                 static_cast<unsigned long>(request_hr), static_cast<unsigned long>(edit_hr),
                 static_cast<unsigned long>(action_hr),
                 static_cast<unsigned int>(text.length()));
        _enqueue_event_trace("composition_commit", detail, FAILED(action_hr));
    }
    const HRESULT final_action_hr = edit_session->action_result();
    edit_session->Release();
    if (FAILED(request_hr) || FAILED(edit_hr)) {
        return FAILED(request_hr) ? request_hr : edit_hr;
    }
    return final_action_hr == E_PENDING ? edit_hr : final_action_hr;
}

HRESULT TextService::_commit_then_restart_composition(ITfContext* context,
                                                      const std::wstring& commit_text,
                                                      const std::wstring& preedit,
                                                      size_t preedit_cursor,
                                                      size_t converted_prefix_utf16,
                                                      size_t focused_start_utf16,
                                                      size_t focused_end_utf16,
                                                      bool focused_converted,
                                                      const std::optional<std::wstring>&
                                                          host_termination_text,
                                                      uint64_t* request_generation) {
    if (!context || commit_text.empty()) {
        const uint64_t generation = begin_composition_edit_request();
        if (request_generation) {
            *request_generation = generation;
        }
        return E_INVALIDARG;
    }
    // Keep commit and restart in one write session: a synchronous restart must not
    // overtake a queued commit, and a failed commit must not apply its suffix.
    return update_composition(context, preedit, preedit_cursor, true, TF_ES_SYNC,
                              converted_prefix_utf16, focused_start_utf16, focused_end_utf16,
                              focused_converted, host_termination_text, nullptr, request_generation,
                              commit_text);
}

void TextService::handle_composition_restart_success(uint64_t expected_generation) {
    if (_candidatePresentation.complete_composition_restart(expected_generation)) {
        _update_state_poll_timer();
    }
}

bool TextService::composition_edit_request_is_current(
    uint64_t expected_generation, uintptr_t expected_context_identity) const {
    return expected_generation != 0 && _compositionEditGeneration == expected_generation &&
           _effectiveEditTarget.valid() && expected_context_identity != 0 &&
           expected_context_identity == _effectiveEditTarget.context_identity;
}

uint64_t TextService::begin_composition_edit_request() {
    // Pending writes share a lifetime and execute in TSF's asynchronous FIFO.
    // A failed earlier write must also cancel its dependent preedit updates.
    if (_pendingCompositionEdits != 0 && _compositionEditGeneration != 0) {
        return _compositionEditGeneration;
    }
    return invalidate_composition_edit_requests();
}

uint64_t TextService::invalidate_composition_edit_requests() {
    ++_compositionEditGeneration;
    if (_compositionEditGeneration == 0) {
        ++_compositionEditGeneration;
    }
    return _compositionEditGeneration;
}

bool TextService::composition_requires_placeholder(const std::wstring& next_text) const {
    // Popup-only compositions need an observable range even when the host returns a
    // plausible rectangle: an empty range can still describe an obsolete insertion point.
    if (!_config.inline_preedit && next_text.empty()) {
        return true;
    }
    return cxxime_tsf::empty_composition_requires_placeholder(
        is_immersive_mode(), _composing && _composition, _lastInlineCompositionText, next_text);
}

bool TextService::handle_composition_edit_failure(uint64_t expected_generation,
                                                  uintptr_t expected_context_identity) {
    // A response can fail after focus has disappeared. Matching the saved lifetime
    // still permits clearing its engine state even when there is no bound context.
    if (expected_generation == 0 || expected_generation != _compositionEditGeneration ||
        expected_context_identity != _effectiveEditTarget.context_identity) {
        return false;
    }
    _enqueue_event_trace("composition_edit", "apply_failed", true);
    if (_sessionId && !_client.clear_composition(_sessionId)) {
        // Never reuse a server session whose buffered input could not be discarded.
        _publish_ui_session_ended();
        _client.disconnect();
        _sessionId = 0;
        _ipcHealthy = false;
    }
    _AbortComposition();
    return true;
}

HRESULT TextService::update_composition(ITfContext* context,
                                         const std::wstring& preedit,
                                         size_t preedit_cursor,
                                         bool ensure,
                                         DWORD edit_session_mode,
                                         size_t converted_prefix_utf16,
                                         size_t focused_start_utf16,
                                         size_t focused_end_utf16,
                                         bool focused_converted,
                                         const std::optional<std::wstring>&
                                             host_termination_text,
                                         bool* host_edit_started,
                                         uint64_t* request_generation,
                                         const std::wstring& commit_before_preedit) {
    const uint64_t generation = begin_composition_edit_request();
    if (request_generation) {
        *request_generation = generation;
    }
    if (host_edit_started) {
        *host_edit_started = _composition != nullptr;
    }
    if (!context) {
        return E_POINTER;
    }
    if (ensure) {
        _ensure_text_edit_sink(context);
    }

    EditSession* edit_session = new (std::nothrow) EditSession(this, context);
    if (!edit_session) {
        return E_OUTOFMEMORY;
    }

    edit_session->set_composition_action(
        !commit_before_preedit.empty() ? EditSession::Action::COMMIT_AND_RESTART_COMPOSITION
        : ensure                       ? EditSession::Action::ENSURE_COMPOSITION_TEXT
                                       : EditSession::Action::UPDATE_COMPOSITION,
        preedit, preedit_cursor, converted_prefix_utf16, focused_start_utf16, focused_end_utf16,
        focused_converted, host_termination_text);
    edit_session->set_commit_before_preedit(commit_before_preedit);
    edit_session->set_composition_edit_request(generation,
                                               _effectiveEditTarget.context_identity);
    if (ensure) {
        edit_session->set_candidate_presentation_request(
            _candidatePresentation.generation(), _effectiveEditTarget.context_identity);
    }

    HRESULT edit_hr = E_FAIL;
    edit_session_mode = ordered_composition_edit_mode(edit_session_mode);
    const bool sync = edit_session_mode == TF_ES_SYNC;
    const DWORD flags = TF_ES_READWRITE | edit_session_mode;
    HRESULT request_hr =
        context->RequestEditSession(_clientId, edit_session, flags, &edit_hr);
    const HRESULT initial_request_hr = request_hr;
    const bool async_fallback = sync && edit_request_can_defer(request_hr, edit_hr) &&
                                edit_session->action_result() == E_PENDING;
    if (async_fallback) {
        edit_hr = E_FAIL;
        request_hr = context->RequestEditSession(
            _clientId, edit_session, TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &edit_hr);
    }

    const HRESULT action_hr = edit_session->action_result();
    cxxime_tsf::TraceCompositionEditResult result;
    result.action = ensure ? "ensure" : "update";
    result.text_length = preedit.size();
    result.selection_offset = preedit_cursor;
    result.sync_requested = sync;
    result.async_fallback = async_fallback;
    result.initial_request_hr = initial_request_hr;
    result.request_hr = request_hr;
    result.edit_hr = edit_hr;
    result.action_hr = action_hr;
    result.start_attempted = edit_session->composition_start_attempted();
    result.start_hr = edit_session->composition_start_result();
    result.composition_returned = edit_session->composition_returned();
    result.composition_active = _composing && _composition != nullptr;
    result.empty_placeholder_active = _emptyCompositionPlaceholderActive;
    cxxime_tsf::trace_composition_edit(this, result);
    if (host_edit_started) {
        // Once deferred, the original key cannot be replayed even if the callback fails.
        const bool deferred = SUCCEEDED(request_hr) && SUCCEEDED(edit_hr) &&
                              action_hr == E_PENDING;
        *host_edit_started = *host_edit_started || result.composition_returned || deferred;
    }
    edit_session->Release();

    if (FAILED(request_hr)) {
        return request_hr;
    }
    if (FAILED(edit_hr)) {
        return edit_hr;
    }
    if ((!sync || async_fallback) && action_hr == E_PENDING) {
        return edit_hr;
    }
    return action_hr;
}

bool TextService::apply_composition_display_attributes(ITfContext* context,
                                                       ITfRange* range,
                                                       TfEditCookie edit_cookie,
                                                       size_t converted_prefix_utf16,
                                                       size_t focused_start_utf16,
                                                       size_t focused_end_utf16,
                                                       bool focused_converted) {
    if (!context || !range || !_displayAttributeAtom) {
        return false;
    }
    if (converted_prefix_utf16 > focused_start_utf16 ||
        focused_start_utf16 > focused_end_utf16 || focused_end_utf16 > LONG_MAX) {
        return false;
    }

    ITfProperty* property = nullptr;
    HRESULT result = context->GetProperty(GUID_PROP_ATTRIBUTE, &property);
    if (FAILED(result) || !property) {
        return false;
    }

    auto apply_atom = [&](ITfRange* target, TfGuidAtom atom) {
        VARIANT value = {};
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = atom;
        const HRESULT set_result = property->SetValue(edit_cookie, target, &value);
        VariantClear(&value);
        return set_result;
    };
    auto apply_range = [&](size_t start, size_t end, TfGuidAtom atom) {
        if (start == end || !atom) {
            return S_OK;
        }
        ITfRange* target = nullptr;
        HRESULT range_result = range->Clone(&target);
        if (FAILED(range_result) || !target) {
            return range_result;
        }
        range_result = target->Collapse(edit_cookie, TF_ANCHOR_START);
        LONG shifted_end = 0;
        if (SUCCEEDED(range_result)) {
            range_result = target->ShiftEnd(edit_cookie, static_cast<LONG>(end), &shifted_end,
                                            nullptr);
        }
        if (SUCCEEDED(range_result) && shifted_end != static_cast<LONG>(end)) {
            range_result = E_INVALIDARG;
        }
        LONG shifted_start = 0;
        if (SUCCEEDED(range_result) && start > 0) {
            range_result = target->ShiftStart(edit_cookie, static_cast<LONG>(start),
                                              &shifted_start, nullptr);
        }
        if (SUCCEEDED(range_result) && shifted_start != static_cast<LONG>(start)) {
            range_result = E_INVALIDARG;
        }
        if (SUCCEEDED(range_result)) {
            range_result = apply_atom(target, atom);
        }
        target->Release();
        return range_result;
    };
    result = apply_atom(range, _displayAttributeAtom);
    if (SUCCEEDED(result)) {
        result = apply_range(0, converted_prefix_utf16, _convertedDisplayAttributeAtom);
    }
    if (SUCCEEDED(result)) {
        const TfGuidAtom focused_atom = focused_converted
                                            ? _focusedConvertedDisplayAttributeAtom
                                            : _focusedDisplayAttributeAtom;
        result = apply_range(focused_start_utf16, focused_end_utf16, focused_atom);
    }
    property->Release();

    if (FAILED(result)) {
        CXXIME_LOG(L"Set composition display attributes failed: hr=0x%08x", result);
        return false;
    }
    return true;
}

HRESULT TextService::_end_composition(ITfContext* context, bool sync) {
    if (!_composition) {
        return S_OK;
    }
    if (!context) {
        return E_FAIL;
    }

    EditSession* edit_session = new (std::nothrow) EditSession(this, context);
    if (!edit_session) {
        return E_OUTOFMEMORY;
    }

    ITfComposition* composition = _composition;
    edit_session->set_end_composition_action(composition);

    HRESULT edit_hr = E_FAIL;
    const DWORD mode = ordered_composition_edit_mode(sync ? TF_ES_SYNC : TF_ES_ASYNC);
    HRESULT request_hr =
        context->RequestEditSession(_clientId, edit_session, TF_ES_READWRITE | mode, &edit_hr);
    if (mode == TF_ES_SYNC && edit_session->action_result() == E_PENDING &&
        edit_request_can_defer(request_hr, edit_hr)) {
        edit_hr = E_FAIL;
        request_hr = context->RequestEditSession(
            _clientId, edit_session, TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &edit_hr);
    }

    edit_session->Release();
    if (_composition == composition) {
        _composition = nullptr;
        set_composition_context(nullptr);
        _composing = false;
        _emptyCompositionPlaceholderActive = false;
        clear_applied_inline_composition_text();
        composition->Release();
    }
    return FAILED(request_hr) ? request_hr : edit_hr;
}

void TextService::_AbortComposition() {
    invalidate_composition_edit_requests();
    _voiceComposition = false;  // ending the composition leaves the voice text in place
    _voiceText.clear();
    ITfComposition* aborted_composition = _composition;
    _hide_candidate_window("hide:abort_composition");
    _end_reading_ui_element("hide:abort_composition_reading");
    clear_applied_inline_composition_text();
    if (_composition) {
        ITfContext* pContext = _current_edit_context_for_composition();
        if (pContext) {
            _end_composition(pContext);
            pContext->Release();
        }
    }
    // EndComposition may synchronously reenter and install a newer composition.
    // Do not let cleanup for the old object overwrite that newer state.
    if (!_composition || _composition == aborted_composition) {
        _composing = false;
        _emptyCompositionPlaceholderActive = false;
        _reset_trace_composition("abort");
    }
}
