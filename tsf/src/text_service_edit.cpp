// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdio>

namespace {

HRESULT range_covers(TfEditCookie edit_cookie, ITfRange* covered_range, ITfRange* covering_range,
                     bool* covered) {
    if (!covered_range || !covering_range || !covered) {
        return E_INVALIDARG;
    }

    *covered = false;
    LONG comparison = 0;
    HRESULT hr =
        covering_range->CompareStart(edit_cookie, covered_range, TF_ANCHOR_START, &comparison);
    if (FAILED(hr) || comparison > 0) {
        return hr;
    }

    hr = covering_range->CompareEnd(edit_cookie, covered_range, TF_ANCHOR_END, &comparison);
    if (SUCCEEDED(hr)) {
        *covered = comparison >= 0;
    }
    return hr;
}

} // namespace

STDMETHODIMP TextService::OnEndEdit(ITfContext* context, TfEditCookie edit_cookie,
                                    ITfEditRecord* edit_record) {
    if (!context || !edit_record) {
        return E_INVALIDARG;
    }
    // The translation window is transient, like the candidate window: an edit (a click that moves
    // the caret) dismisses it.
    _hide_translate_window();
    if (!_composing || !_composition || context != _textEditSinkContext) {
        return S_OK;
    }

    BOOL selection_changed = FALSE;
    HRESULT hr = edit_record->GetSelectionStatus(&selection_changed);
    if (FAILED(hr)) {
        char detail[112] = {};
        snprintf(detail, sizeof(detail), "selection=unknown step=get_status hr=0x%08x action=keep",
                 static_cast<unsigned int>(hr));
        _enqueue_event_trace("text_edit", detail, true);
        return S_OK;
    }
    if (!selection_changed) {
        return S_OK;
    }

    BOOL own_write_session = FALSE;
    const HRESULT write_session_hr = context->InWriteSession(_clientId, &own_write_session);
    if (SUCCEEDED(write_session_hr) && own_write_session) {
        _enqueue_event_trace("text_edit", "selection=changed source=self action=keep");
        return S_OK;
    }

    TF_SELECTION selection = {};
    ULONG fetched = 0;
    hr = context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || !selection.range) {
        if (selection.range) {
            selection.range->Release();
        }
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "selection=unavailable get_hr=0x%08x action=keep",
                 static_cast<unsigned int>(hr));
        _enqueue_event_trace("text_edit", detail, true);
        return S_OK;
    }

    ITfRange* composition_range = nullptr;
    const HRESULT composition_range_hr = _composition->GetRange(&composition_range);
    bool selection_inside = false;
    HRESULT compare_hr = E_FAIL;
    if (SUCCEEDED(composition_range_hr) && composition_range) {
        compare_hr =
            range_covers(edit_cookie, selection.range, composition_range, &selection_inside);
        composition_range->Release();
    }
    selection.range->Release();

    if (FAILED(composition_range_hr) || FAILED(compare_hr)) {
        char detail[160] = {};
        snprintf(detail, sizeof(detail),
                 "selection=unknown range_hr=0x%08x compare_hr=0x%08x action=keep",
                 static_cast<unsigned int>(composition_range_hr),
                 static_cast<unsigned int>(compare_hr));
        _enqueue_event_trace("text_edit", detail, true);
        return S_OK;
    }
    if (selection_inside) {
        _enqueue_event_trace("text_edit", "selection=inside source=host action=keep");
        return S_OK;
    }

    bool clear_succeeded = false;
    if (_sessionId) {
        clear_succeeded = _client.clear_composition(_sessionId);
    }
    char detail[160] = {};
    snprintf(detail, sizeof(detail),
             "selection=outside source=host action=cancel session=%u clear_succeeded=%d "
             "write_session_hr=0x%08x",
             _sessionId, clear_succeeded ? 1 : 0, static_cast<unsigned int>(write_session_hr));
    _enqueue_event_trace("text_edit", detail, true);
    _AbortComposition();
    return S_OK;
}

bool TextService::_ensure_text_edit_sink(ITfContext* context) {
    if (!context) {
        return false;
    }
    if (_textEditSinkContext == context && _dwTextEditSinkCookie != TF_INVALID_COOKIE) {
        return true;
    }

    return _bind_text_edit_sink(context);
}

bool TextService::_bind_text_edit_sink(ITfContext* context) {
    _unadvise_text_edit_sink();
    if (!context) {
        return true;
    }

    ITfSource* source = nullptr;
    HRESULT hr = context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (FAILED(hr) || !source) {
        char detail[112] = {};
        snprintf(detail, sizeof(detail), "action=advise step=query_source hr=0x%08x",
                 static_cast<unsigned int>(hr));
        _enqueue_event_trace("text_edit_sink", detail, true);
        return false;
    }

    DWORD cookie = TF_INVALID_COOKIE;
    hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &cookie);
    source->Release();
    if (FAILED(hr)) {
        char detail[96] = {};
        snprintf(detail, sizeof(detail), "action=advise hr=0x%08x", static_cast<unsigned int>(hr));
        _enqueue_event_trace("text_edit_sink", detail, true);
        return false;
    }

    context->AddRef();
    _textEditSinkContext = context;
    _dwTextEditSinkCookie = cookie;
    _enqueue_event_trace("text_edit_sink", "action=advise hr=0x00000000");
    return true;
}

void TextService::_unadvise_text_edit_sink() {
    HRESULT hr = S_FALSE;
    if (_textEditSinkContext) {
        ITfSource* source = nullptr;
        if (_dwTextEditSinkCookie != TF_INVALID_COOKIE &&
            SUCCEEDED(_textEditSinkContext->QueryInterface(
                IID_ITfSource, reinterpret_cast<void**>(&source))) && source) {
            hr = source->UnadviseSink(_dwTextEditSinkCookie);
            source->Release();
        }
        _textEditSinkContext->Release();
        _textEditSinkContext = nullptr;
    }
    _dwTextEditSinkCookie = TF_INVALID_COOKIE;

    if (hr != S_FALSE) {
        char detail[96] = {};
        snprintf(detail, sizeof(detail), "action=unadvise hr=0x%08x",
                 static_cast<unsigned int>(hr));
        _enqueue_event_trace("text_edit_sink", detail, FAILED(hr));
    }
}
