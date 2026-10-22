// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "edit_session.h"

#include <climits>

#include <cxxime/diagnostics_config.h>

#include "edit_target.h"
#include "globals.h"
#include "text_service.h"

namespace {

HRESULT set_composition_range_text(ITfRange* range, TfEditCookie edit_cookie,
                                   const std::wstring& text, bool use_empty_placeholder) {
    if (!range) {
        return E_INVALIDARG;
    }

    // Keep a nonempty host range when the composition policy requires a placeholder.
    // A successful geometry query alone does not guarantee an up-to-date insertion point.
    const bool store_placeholder = text.empty() && use_empty_placeholder;
    const wchar_t* stored_text = store_placeholder ? L" " : text.c_str();
    const LONG stored_length = store_placeholder ? 1 : static_cast<LONG>(text.length());
    const DWORD flags = store_placeholder ? TF_ST_CORRECTION : 0;
    return range->SetText(edit_cookie, flags, stored_text, stored_length);
}

bool is_placeholder_caret_rect(ITfContext* context, const RECT& caret_rect) {
    ITfContextView* view = nullptr;
    RECT view_rect = {};
    const bool has_view_rect = context &&
        SUCCEEDED(context->GetActiveView(&view)) && view &&
        SUCCEEDED(view->GetScreenExt(&view_rect));
    if (view) {
        view->Release();
    }

    HWND foreground = GetForegroundWindow();
    RECT foreground_rect = {};
    const bool has_foreground_rect =
        foreground && GetWindowRect(foreground, &foreground_rect);
    return (has_view_rect &&
            cxxime_tsf::text_rect_requires_composition_refresh(view_rect, caret_rect)) ||
           (has_foreground_rect && cxxime_tsf::text_rect_requires_composition_refresh(
                                    foreground_rect, caret_rect));
}

// The whole text being composed, as the caret (a host can answer an empty range badly and the
// composed text well). False when not composing or it lies outside `foreground`.
bool composition_text_rect(TextService* service, TfEditCookie ec, ITfContextView* view,
                           HWND foreground, RECT* out, cxxime_tsf::TextExtRectTrace* trace) {
    ITfComposition* composition =
        service && service->is_composing() ? service->get_composition() : nullptr;
    ITfRange* range = nullptr;
    if (!composition || !view || FAILED(composition->GetRange(&range)) || !range) {
        return false;
    }
    RECT rect = {};
    BOOL clipped = FALSE;
    const HRESULT hr = view->GetTextExt(ec, range, &rect, &clipped);
    range->Release();
    if (trace) {
        trace->composition_hr = hr;
        trace->composition_rect = rect;
    }
    RECT window = {};
    if (FAILED(hr) || rect.bottom <= rect.top || rect.right < rect.left || !foreground ||
        !GetWindowRect(foreground, &window) || rect.left < window.left ||
        rect.left >= window.right || rect.top < window.top || rect.top >= window.bottom) {
        return false;
    }
    SetRect(out, rect.left, rect.top, rect.left + 1, rect.bottom);
    return true;
}

struct RangeCaretResult {
    RECT rect = {};
    RECT view_rect = {};
    bool viewport_fallback = false;
    bool remember_visible = false;
};

bool get_range_caret_rect(TextService* service,
                          ITfContext* context,
                          TfEditCookie ec,
                          ITfRange* range,
                          TfAnchor anchor,
                          RangeCaretResult* out) {
    if (!context || !range || !out)
        return false;

    ITfContextView* pView = nullptr;
    if (FAILED(context->GetActiveView(&pView)) || !pView)
        return false;

    ITfRange* caret_range = nullptr;
    if (SUCCEEDED(range->Clone(&caret_range)) && caret_range) {
        caret_range->Collapse(ec, anchor);
    } else {
        caret_range = range;
        caret_range->AddRef();
    }

    RECT rc = {};
    BOOL clipped = FALSE;
    const HRESULT text_ext_hr = pView->GetTextExt(ec, caret_range, &rc, &clipped);
    const RECT raw_rect = rc;
    RECT view_rect = {};
    const HRESULT view_rect_hr = pView->GetScreenExt(&view_rect);
    const bool has_view_rect = SUCCEEDED(view_rect_hr) && view_rect.right > view_rect.left &&
                               view_rect.bottom > view_rect.top;
    bool resolved = SUCCEEDED(text_ext_hr);
    cxxime_tsf::CaretViewportFallback viewport_fallback =
        cxxime_tsf::CaretViewportFallback::None;
    cxxime_tsf::TextExtRectTrace trace;
    const auto trace_mode = cxxime::diagnostics_config().trace_mode;
    const bool trace_enabled = service && trace_mode >= cxxime::DiagnosticTraceMode::kNormal;
    if (trace_enabled) {
        trace.raw = rc;
        trace.text_ext_hr = text_ext_hr;
        trace.clipped = clipped != FALSE;
    }
    HWND view_hwnd = nullptr;
    pView->GetWnd(&view_hwnd);
    HWND foreground = GetForegroundWindow();
    if (resolved) {
        if (trace_enabled) {
            trace.view_hwnd = view_hwnd;
            trace.foreground_hwnd = foreground;
        }
        resolved = cxxime_tsf::normalize_text_ext_rect(
            view_hwnd, foreground, &rc, trace_enabled ? &trace : nullptr);
    }
    // GetTextExt is defined in screen coordinates. Prefer a successfully normalized host
    // rectangle, but retain the contractual raw geometry when it lies beyond all monitors.
    const RECT logical_rect = resolved ? rc : raw_rect;
    const bool has_logical_rect = SUCCEEDED(text_ext_hr);
    if (resolved && has_view_rect &&
        cxxime_tsf::text_rect_is_outside_view(S_OK, view_rect, S_OK, rc, false)) {
        resolved = false;
    }
    // No caret, or one with no height (issue #24: Age of Empires II DE answers its window's
    // bottom-right corner, where Microsoft Pinyin still finds the chat box): the composed text
    // as a whole, the foreground thread's system caret, then where the program asked for the
    // candidate window through IMM.
    if (!resolved || raw_rect.bottom == raw_rect.top) {
        cxxime_tsf::TextExtRectTrace* alternative_trace = trace_enabled ? &trace : nullptr;
        if (trace_enabled) {
            trace.alternatives = true;
        }
        RECT alternative = {};
        HWND focus = nullptr;
        const char* branch = nullptr;
        if (composition_text_rect(service, ec, pView, foreground, &alternative,
                                  alternative_trace)) {
            branch = "composition_text";
        } else if (cxxime_tsf::foreground_caret_rect(foreground, &alternative, &focus,
                                                     alternative_trace)) {
            branch = "foreground_caret";
        } else if (cxxime_tsf::imm_candidate_caret_rect(
                       focus ? focus : (view_hwnd ? view_hwnd : foreground), &alternative,
                       alternative_trace)) {
            branch = "imm_candidate_form";
        }
        if (branch) {
            rc = alternative;
            resolved = true;
            if (trace_enabled) {
                trace.branch = branch;
            }
        }
    }
    if (!resolved && service) {
        viewport_fallback = service->resolve_viewport_caret(
            has_view_rect ? &view_rect : nullptr,
            has_logical_rect ? &logical_rect : nullptr,
            clipped != FALSE, &rc);
        resolved = viewport_fallback != cxxime_tsf::CaretViewportFallback::None;
        if (trace_enabled && resolved) {
            switch (viewport_fallback) {
            case cxxime_tsf::CaretViewportFallback::Projected:
                trace.branch = "viewport_projection";
                break;
            case cxxime_tsf::CaretViewportFallback::Boundary:
                trace.branch = "viewport_boundary";
                break;
            default:
                trace.branch = "viewport_anchor";
                break;
            }
        }
    }
    if (trace_enabled) {
        trace.result = rc;
        trace.resolved = resolved;
        service->trace_text_ext_rect(trace);
    }
    if (resolved) {
        out->rect = rc;
        out->view_rect = view_rect;
        out->viewport_fallback = viewport_fallback != cxxime_tsf::CaretViewportFallback::None;
        out->remember_visible = !out->viewport_fallback && has_view_rect && clipped == FALSE;
    }

    caret_range->Release();
    pView->Release();
    return resolved;
}

bool resolve_caret_rect_from_range(TextService* service,
                                   ITfContext* context,
                                   TfEditCookie ec,
                                   ITfRange* range,
                                   TfAnchor anchor,
                                   RECT* out,
                                   bool* viewport_fallback_out = nullptr) {
    RangeCaretResult result;
    if (!get_range_caret_rect(service, context, ec, range, anchor, &result))
        return false;
    if (!result.viewport_fallback && is_placeholder_caret_rect(context, result.rect)) {
        if (service) {
            service->trace_caret_event("reject", "placeholder", false, &result.rect, S_FALSE,
                                       true);
        }
        return false;
    }

    if (service && result.remember_visible) {
        service->remember_viewport_caret(result.view_rect, result.rect);
    }
    if (out)
        *out = result.rect;
    if (viewport_fallback_out)
        *viewport_fallback_out = result.viewport_fallback;
    return true;
}

bool resolve_caret_rect_from_selection(TextService* service,
                                       ITfContext* context,
                                       TfEditCookie ec,
                                       RECT* out,
                                       bool* viewport_fallback_out = nullptr) {
    if (!context)
        return false;

    TF_SELECTION selection = {};
    ULONG fetched = 0;
    HRESULT hr = context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched == 0 || !selection.range)
        return false;

    bool resolved = resolve_caret_rect_from_range(service, context, ec, selection.range,
                                                 TF_ANCHOR_END, out,
                                                 viewport_fallback_out);
    selection.range->Release();
    return resolved;
}

bool resolve_caret_rect_from_composition(TextService* service,
                                       ITfContext* context,
                                       TfEditCookie ec,
                                       RECT* out,
                                       const char** source_out = nullptr,
                                       bool* viewport_fallback_out = nullptr) {
    ITfComposition* composition =
        service && service->is_composing() ? service->get_composition() : nullptr;
    ITfRange* range = nullptr;
    if (!composition || FAILED(composition->GetRange(&range)) || !range)
        return false;

    RECT rc = {};
    bool viewport_fallback = false;
    bool resolved = resolve_caret_rect_from_range(service, context, ec, range,
                                                  TF_ANCHOR_END, &rc, &viewport_fallback);
    const char* source = "composition_end";
    if (!resolved) {
        resolved = resolve_caret_rect_from_range(service, context, ec, range,
                                                 TF_ANCHOR_START, &rc, &viewport_fallback);
        source = "composition_start";
    }
    range->Release();
    if (!resolved)
        return false;

    if (out)
        *out = rc;
    if (source_out)
        *source_out = source;
    if (viewport_fallback_out)
        *viewport_fallback_out = viewport_fallback;
    return true;
}

void update_caret_rect(TextService* service, ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!service || !context)
        return;

    RECT rc = {};
    if (resolve_caret_rect_from_selection(service, context, ec, &rc) ||
        resolve_caret_rect_from_range(service, context, ec, range, TF_ANCHOR_END, &rc)) {
        service->set_caret_rect(rc);
    }
}

bool resolve_current_caret_rect(TextService* service,
                                ITfContext* context,
                                TfEditCookie ec,
                                RECT* out,
                                const char** source_out = nullptr,
                                bool* viewport_fallback_out = nullptr) {
    RECT rc = {};
    bool viewport_fallback = false;
    if (resolve_caret_rect_from_selection(service, context, ec, &rc,
                                          &viewport_fallback)) {
        if (out)
            *out = rc;
        if (source_out)
            *source_out = viewport_fallback ? "selection_viewport" : "selection";
        if (viewport_fallback_out)
            *viewport_fallback_out = viewport_fallback;
        return true;
    }

    if (resolve_caret_rect_from_composition(service, context, ec, &rc, source_out,
                                            &viewport_fallback)) {
        if (out)
            *out = rc;
        if (viewport_fallback_out)
            *viewport_fallback_out = viewport_fallback;
        return true;
    }

    if (source_out)
        *source_out = "none";
    return false;
}

HRESULT set_selection_to_range(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range)
        return E_INVALIDARG;

    ITfRange* selection_range = nullptr;
    HRESULT hr = range->Clone(&selection_range);
    if (FAILED(hr) || !selection_range)
        return FAILED(hr) ? hr : E_FAIL;

    hr = selection_range->Collapse(ec, TF_ANCHOR_END);
    if (FAILED(hr)) {
        selection_range->Release();
        return hr;
    }

    TF_SELECTION selection = {};
    selection.range = selection_range;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    hr = context->SetSelection(ec, 1, &selection);
    selection_range->Release();
    return hr;
}

HRESULT set_selection_to_range_offset(ITfContext* context, TfEditCookie ec, ITfRange* range,
                                      size_t offset) {
    if (!context || !range || offset > static_cast<size_t>(LONG_MAX)) {
        return E_INVALIDARG;
    }

    ITfRange* selection_range = nullptr;
    HRESULT hr = range->Clone(&selection_range);
    if (FAILED(hr) || !selection_range) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = selection_range->Collapse(ec, TF_ANCHOR_START);
    if (SUCCEEDED(hr) && offset > 0) {
        LONG shifted = 0;
        hr = selection_range->ShiftStart(ec, static_cast<LONG>(offset), &shifted, nullptr);
        if (SUCCEEDED(hr) && shifted != static_cast<LONG>(offset)) {
            hr = E_INVALIDARG;
        }
    }

    if (SUCCEEDED(hr)) {
        TF_SELECTION selection = {};
        selection.range = selection_range;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        hr = context->SetSelection(ec, 1, &selection);
    }
    selection_range->Release();
    return hr;
}

void clear_display_attribute(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range)
        return;

    ITfProperty* property = nullptr;
    if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)) && property) {
        property->Clear(ec, range);
        property->Release();
    }
}

void set_composition_language(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range)
        return;

    ITfProperty* property = nullptr;
    if (FAILED(context->GetProperty(GUID_PROP_LANGID, &property)) || !property)
        return;

    VARIANT value = {};
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = TEXTSERVICE_LANGID_HANS;
    property->SetValue(ec, range, &value);
    VariantClear(&value);
    property->Release();
}

HRESULT create_composition(TextService* service,
                           ITfContext* context,
                           TfEditCookie ec,
                           ITfRange** range_out,
                           bool* start_attempted,
                           HRESULT* start_result,
                           bool* composition_returned) {
    if (!service || !context || !range_out || !start_attempted || !start_result ||
        !composition_returned) {
        return E_INVALIDARG;
    }
    *range_out = nullptr;
    *start_attempted = false;
    *start_result = E_PENDING;
    *composition_returned = false;

    ITfInsertAtSelection* insert_at_selection = nullptr;
    HRESULT hr = context->QueryInterface(IID_ITfInsertAtSelection,
                                         reinterpret_cast<void**>(&insert_at_selection));
    if (FAILED(hr) || !insert_at_selection) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }

    ITfRange* range = nullptr;
    hr = insert_at_selection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
    insert_at_selection->Release();
    if (FAILED(hr) || !range) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    ITfContextComposition* context_composition = nullptr;
    hr = context->QueryInterface(IID_ITfContextComposition,
                                 reinterpret_cast<void**>(&context_composition));
    if (FAILED(hr) || !context_composition) {
        range->Release();
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }

    ITfComposition* composition = nullptr;
    *start_attempted = true;
    hr = context_composition->StartComposition(ec, range, service, &composition);
    *start_result = hr;
    *composition_returned = composition != nullptr;
    context_composition->Release();
    if (FAILED(hr) || !composition) {
        range->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    service->set_composition(composition);
    service->set_composition_context(context);
    service->set_composing(true);
    service->set_empty_composition_placeholder_active(false);
    *range_out = range;
    return S_OK;
}

HRESULT get_or_create_composition_range(TextService* service,
                                        ITfContext* context,
                                        TfEditCookie ec,
                                        ITfRange** range_out,
                                        bool* start_attempted,
                                        HRESULT* start_result,
                                        bool* composition_returned) {
    if (!service || !range_out || !start_attempted || !start_result ||
        !composition_returned) {
        return E_INVALIDARG;
    }
    *range_out = nullptr;
    *start_attempted = false;
    *start_result = E_PENDING;
    *composition_returned = false;

    ITfComposition* composition = service->get_composition();
    if (composition && service->is_composing()) {
        const HRESULT range_result = composition->GetRange(range_out);
        if (FAILED(range_result) || !*range_out) {
            return FAILED(range_result) ? range_result : E_FAIL;
        }
        return range_result;
    }

    return create_composition(service, context, ec, range_out, start_attempted,
                              start_result, composition_returned);
}

HRESULT clear_and_end_composition(TextService* service,
                                  ITfContext* context,
                                  TfEditCookie ec,
                                  const std::wstring* commit_text,
                                  ITfComposition* expected_composition = nullptr,
                                  bool preserve_text = false) {
    ITfComposition* composition =
        expected_composition ? expected_composition
                             : (service ? service->get_composition() : nullptr);
    if (!service || !composition)
        return E_INVALIDARG;

    const bool current_composition = service->get_composition() == composition;

    HRESULT action_result = S_OK;
    ITfRange* committed_end = nullptr;
    ITfRange* range = nullptr;
    HRESULT hr = composition->GetRange(&range);
    if (SUCCEEDED(hr) && range) {
        const wchar_t* text = commit_text ? commit_text->c_str() : L"";
        LONG length = commit_text ? static_cast<LONG>(commit_text->length()) : 0;
        if (!preserve_text) {
            clear_display_attribute(context, ec, range);
            action_result = range->SetText(ec, 0, text, length);
        }

        if (SUCCEEDED(action_result) && commit_text && length > 0) {
            action_result = range->Clone(&committed_end);
            if (SUCCEEDED(action_result) && committed_end) {
                action_result = committed_end->Collapse(ec, TF_ANCHOR_END);
            } else if (SUCCEEDED(action_result)) {
                action_result = E_FAIL;
            }
        }
        range->Release();
    } else {
        action_result = FAILED(hr) ? hr : E_FAIL;
    }

    if (current_composition) {
        service->set_composition(nullptr);
        service->set_composition_context(nullptr);
        service->set_composing(false);
        service->set_empty_composition_placeholder_active(false);
        service->set_applied_inline_composition_text(L"");
    }
    hr = composition->EndComposition(ec);
    if (SUCCEEDED(action_result) && FAILED(hr)) {
        action_result = hr;
    }
    if (current_composition)
        composition->Release();

    // EndComposition can reset the host selection. Apply the committed caret afterwards.
    if (SUCCEEDED(action_result) && committed_end) {
        hr = set_selection_to_range(context, ec, committed_end);
        if (FAILED(hr))
            action_result = hr;
    }
    if (committed_end)
        committed_end->Release();
    return action_result;
}

HRESULT apply_composition_text(TextService* service, ITfContext* context, TfEditCookie ec,
                               ITfRange* range, const std::wstring& text, size_t selection_offset,
                               bool has_selection_offset, size_t converted_prefix_utf16,
                               size_t focused_start_utf16, size_t focused_end_utf16,
                               bool focused_converted,
                               const std::optional<std::wstring>& host_termination_text,
                               bool composition_started,
                               bool* text_written) {
    *text_written = false;
    if (!service || !context || !range) {
        return E_INVALIDARG;
    }

    const bool placeholder_already_active =
        service->empty_composition_placeholder_active();
    const bool preserve_empty_composition = service->composition_requires_placeholder(text);
    bool caret_resolved_before_write = false;
    if (text.empty() && !placeholder_already_active && !preserve_empty_composition) {
        RECT caret_rect = {};
        caret_resolved_before_write =
            resolve_caret_rect_from_selection(service, context, ec, &caret_rect);
        if (caret_resolved_before_write) {
            service->set_caret_rect(caret_rect);
        }
    }
    const bool use_empty_placeholder =
        text.empty() && (placeholder_already_active || preserve_empty_composition ||
        (composition_started && !caret_resolved_before_write));
    HRESULT result =
        set_composition_range_text(range, ec, text, use_empty_placeholder);
    if (FAILED(result)) {
        return result;
    }
    *text_written = true;
    service->set_empty_composition_placeholder_active(use_empty_placeholder);
    service->set_applied_inline_composition_text(text, host_termination_text);

    set_composition_language(context, ec, range);
    service->apply_composition_display_attributes(
        context, range, ec, converted_prefix_utf16, focused_start_utf16,
        focused_end_utf16, focused_converted);
    result = has_selection_offset
        ? set_selection_to_range_offset(context, ec, range, selection_offset)
        : set_selection_to_range(context, ec, range);
    if (!caret_resolved_before_write) {
        update_caret_rect(service, context, ec, range);
    }
    return result;
}

HRESULT insert_at_selection(ITfContext* context,
                            TfEditCookie ec,
                            const std::wstring& text) {
    if (!context || text.empty())
        return E_INVALIDARG;

    ITfInsertAtSelection* insert_at_selection = nullptr;
    HRESULT hr = context->QueryInterface(
        IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert_at_selection));
    if (FAILED(hr) || !insert_at_selection) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    ITfRange* pRange = nullptr;
    hr = insert_at_selection->InsertTextAtSelection(
        ec, TF_IAS_QUERYONLY, nullptr, 0, &pRange);
    insert_at_selection->Release();
    if (FAILED(hr) || !pRange) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = pRange->SetText(
        ec, TF_ST_CORRECTION, text.c_str(), static_cast<LONG>(text.length()));
    if (SUCCEEDED(hr)) {
        hr = set_selection_to_range(context, ec, pRange);
    }
    pRange->Release();
    return hr;
}

HRESULT commit_to_context(TextService* service, ITfContext* context, TfEditCookie ec,
                          const std::wstring& text) {
    ITfContext* composition_context = service->get_composition_context();
    IUnknown* target_identity = nullptr;
    IUnknown* composition_identity = nullptr;
    if (context) {
        context->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&target_identity));
    }
    if (composition_context) {
        composition_context->QueryInterface(IID_IUnknown,
                                            reinterpret_cast<void**>(&composition_identity));
    }
    const bool matches = target_identity && target_identity == composition_identity;
    if (target_identity) {
        target_identity->Release();
    }
    if (composition_identity) {
        composition_identity->Release();
    }
    return matches && service->get_composition()
        ? clear_and_end_composition(service, context, ec, &text)
        : insert_at_selection(context, ec, text);
}

} // namespace

EditSession::EditSession(TextService* service, ITfContext* context)
    : _service(service), _context(context) {
    if (_service)
        _service->AddRef();
    if (_context)
        _context->AddRef();
}

EditSession::~EditSession() {
    if (_registeredWrite) {
        _service->release_composition_edit();
    }
    if (_expectedComposition)
        _expectedComposition->Release();
    if (_context)
        _context->Release();
    if (_service)
        _service->Release();
}

STDMETHODIMP EditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EditSession::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) EditSession::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

void EditSession::set_action(Action action, const std::wstring& text) {
    if (_registeredWrite) {
        _service->release_composition_edit();
        _registeredWrite = false;
    }
    if (_service && action != Action::QUERY_CARET && action != Action::UPDATE_CANDIDATE_POSITION) {
        _service->register_composition_edit();
        _registeredWrite = true;
    }
    if (_expectedComposition) {
        _expectedComposition->Release();
        _expectedComposition = nullptr;
    }
    _action = action;
    _text = text;
    _selectionOffset = 0;
    _hasSelectionOffset = false;
    _convertedPrefixUtf16 = 0;
    _focusedStartUtf16 = 0;
    _focusedEndUtf16 = 0;
    _focusedConverted = false;
    _hostTerminationText.reset();
    _actionResult = E_PENDING;
    _compositionStartAttempted = false;
    _compositionStartResult = E_PENDING;
    _compositionReturned = false;
}

void EditSession::set_end_composition_action(ITfComposition* composition) {
    set_action(Action::END_COMPOSITION);
    _expectedComposition = composition;
    if (_expectedComposition)
        _expectedComposition->AddRef();
}

void EditSession::set_composition_action(Action action, const std::wstring& text,
                                         size_t selection_offset,
                                         size_t converted_prefix_utf16,
                                         size_t focused_start_utf16,
                                         size_t focused_end_utf16,
                                         bool focused_converted,
                                         const std::optional<std::wstring>&
                                             host_termination_text) {
    set_action(action, text);
    _selectionOffset = selection_offset;
    _hasSelectionOffset = true;
    _convertedPrefixUtf16 = converted_prefix_utf16;
    _focusedStartUtf16 = focused_start_utf16;
    _focusedEndUtf16 = focused_end_utf16;
    _focusedConverted = focused_converted;
    _hostTerminationText = host_termination_text;
}

STDMETHODIMP EditSession::DoEditSession(TfEditCookie ec) {
    if (_action == Action::INSERT_TEXT && !_text.empty()) {
        _actionResult = insert_at_selection(_context, ec, _text);
        if (FAILED(_actionResult) && _service) {
            _service->handle_composition_edit_failure(
                _compositionEditGeneration, _compositionEditContextIdentity);
        }
} else if (_action == Action::END_COMPOSITION) {
        _actionResult = clear_and_end_composition(
            _service, _context, ec, nullptr, _expectedComposition);
} else if (_action == Action::UPDATE_COMPOSITION) {
        if (!_service->composition_edit_request_is_current(
                _compositionEditGeneration, _compositionEditContextIdentity)) {
            _actionResult = S_FALSE;
            return S_OK;
        }
        ITfComposition* pComp = _service->get_composition();
        ITfRange* pRange = nullptr;
        _actionResult = pComp ? pComp->GetRange(&pRange) : E_UNEXPECTED;
        if (SUCCEEDED(_actionResult) && pRange) {
            _actionResult = pRange->SetText(
                ec, 0, _text.c_str(), static_cast<LONG>(_text.length()));
            if (SUCCEEDED(_actionResult)) {
                _service->set_applied_inline_composition_text(_text, _hostTerminationText);
                set_composition_language(_context, ec, pRange);
                _service->apply_composition_display_attributes(
                    _context, pRange, ec, _convertedPrefixUtf16, _focusedStartUtf16,
                    _focusedEndUtf16, _focusedConverted);
                _actionResult = _hasSelectionOffset
                    ? set_selection_to_range_offset(
                        _context, ec, pRange, _selectionOffset)
                    : set_selection_to_range(_context, ec, pRange);
            }
        } else {
            TF_SELECTION sel = {};
            ULONG fetched = 0;
            if (SUCCEEDED(_context->GetSelection(
                    ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched > 0) {
                pRange = sel.range;
            }
        }
        if (pRange) {
            update_caret_rect(_service, _context, ec, pRange);
            pRange->Release();
        }
    } else if (_action == Action::ENSURE_COMPOSITION_TEXT ||
               _action == Action::COMMIT_AND_RESTART_COMPOSITION) {
        if (!_service ||
            (_action == Action::ENSURE_COMPOSITION_TEXT &&
             !_service->composition_edit_request_is_current(
                 _compositionEditGeneration, _compositionEditContextIdentity))) {
            _actionResult = S_FALSE;
        } else {
            _actionResult = S_OK;
            if (_action == Action::COMMIT_AND_RESTART_COMPOSITION) {
                _actionResult = commit_to_context(_service, _context, ec, _commitBeforePreedit);
            }
            ITfRange* range = nullptr;
            bool composition_text_written = false;
            if (SUCCEEDED(_actionResult) &&
                _service->composition_edit_request_is_current(_compositionEditGeneration,
                                                              _compositionEditContextIdentity)) {
                _actionResult = get_or_create_composition_range(
                    _service, _context, ec, &range, &_compositionStartAttempted,
                    &_compositionStartResult, &_compositionReturned);
            }
                if (SUCCEEDED(_actionResult) && range) {
                    _actionResult = apply_composition_text(
                        _service, _context, ec, range, _text, _selectionOffset,
                        _hasSelectionOffset, _convertedPrefixUtf16, _focusedStartUtf16,
                        _focusedEndUtf16, _focusedConverted, _hostTerminationText,
                        _compositionStartAttempted, &composition_text_written);
                    range->Release();
                }
                if (SUCCEEDED(_actionResult) && range) {
                    _service->handle_composition_restart_success(
                        _candidatePresentationGeneration);
                } else if (FAILED(_actionResult) &&
                           _service->composition_edit_request_is_current(
                               _compositionEditGeneration, _compositionEditContextIdentity)) {
                    // Use the granted write cookie to remove a partially applied composition.
                    // A stale callback must not end or clear a newer composition/session.
                    if (_service->get_composition()) {
                        // A new range can cover the host's existing selection. If our first
                        // write failed, end it without deleting that original host text.
                        const bool preserve_text =
                            _compositionReturned && !composition_text_written;
                        clear_and_end_composition(_service, _context, ec, nullptr, nullptr,
                                                  preserve_text);
                    }
                    _service->handle_composition_edit_failure(
                        _compositionEditGeneration, _compositionEditContextIdentity);
                }
        }
    } else if (_action == Action::COMMIT_COMPOSITION) {
        _actionResult = commit_to_context(_service, _context, ec, _text);
        if (FAILED(_actionResult)) {
            _service->handle_composition_edit_failure(
                _compositionEditGeneration, _compositionEditContextIdentity);
        }
    } else if (_action == Action::QUERY_CARET) {
        RECT rc = {};
        const char* source = "none";
        bool viewport_fallback = false;
        if (resolve_current_caret_rect(_service, _context, ec, &rc, &source,
                                       &viewport_fallback)) {
            _resultRect = rc;
            _resultValid = true;
            _resultUsesViewportFallback = viewport_fallback;
            if (_service) {
                _service->trace_caret_event("query", source, true, &rc);
            }
        } else if (_service) {
            _service->trace_caret_event("query", source, false, nullptr, E_FAIL, true);
        }
    } else if (_action == Action::UPDATE_CANDIDATE_POSITION) {
        RECT rc = {};
        const char* source = "none";
        bool viewport_fallback = false;
        if (resolve_current_caret_rect(_service, _context, ec, &rc, &source,
                                       &viewport_fallback)) {
            _resultRect = rc;
            _resultValid = true;
            _resultUsesViewportFallback = viewport_fallback;
            if (_service) {
                _service->trace_caret_event("layout_update", source, true, &rc);
                _service->update_candidate_position(rc, _context,
                                                    _positionUpdateFromLayoutChange,
                                                    _candidatePresentationGeneration,
                                                    viewport_fallback);
            }
        } else if (_service) {
            _service->trace_caret_event("layout_update", source, false, nullptr, E_FAIL, true);
        }
    }
    return S_OK;
}
