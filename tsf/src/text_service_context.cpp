// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

void TextService::_refresh_ui_presentation(const char* reason) {
    _enqueue_event_trace("ui_presentation", reason);
    _publish_ui_presentation();
}

bool TextService::_context_belongs_to_foreground(ITfContext* context) const {
    if (!context)
        return false;

    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;

    HWND context_hwnd = nullptr;
    ITfContextView* view = nullptr;
    if (SUCCEEDED(context->GetActiveView(&view)) && view) {
        view->GetWnd(&context_hwnd);
        view->Release();
    }

    if (context_hwnd == foreground || IsChild(foreground, context_hwnd)) {
        return true;
    }

    if (context_hwnd) {
        HWND context_root = GetAncestor(context_hwnd, GA_ROOT);
        HWND foreground_root = GetAncestor(foreground, GA_ROOT);
        if (context_root && context_root == foreground_root) {
            return true;
        }
    }

    // TSF supports windowless contexts and hosts whose view geometry is not ready.
    // Use the foreground process and the thread-manager focus identity as the
    // ownership check instead of requiring GetWnd/GetScreenExt to succeed.
    DWORD foreground_process = 0;
    GetWindowThreadProcessId(foreground, &foreground_process);
    if (foreground_process != GetCurrentProcessId() || !_threadMgr) {
        return false;
    }

    ITfDocumentMgr* document = nullptr;
    ITfContext* focused_context = nullptr;
    if (SUCCEEDED(_threadMgr->GetFocus(&document)) && document) {
        document->GetTop(&focused_context);
    }
    if (document) {
        document->Release();
    }
    IUnknown* context_identity = nullptr;
    IUnknown* focus_identity = nullptr;
    context->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&context_identity));
    if (focused_context) {
        focused_context->QueryInterface(IID_IUnknown,
                                        reinterpret_cast<void**>(&focus_identity));
        focused_context->Release();
    }
    const bool matches = context_identity && context_identity == focus_identity;
    if (context_identity) {
        context_identity->Release();
    }
    if (focus_identity) {
        focus_identity->Release();
    }
    return matches;
}

bool TextService::_read_context_compartment_bool(ITfContext* context, REFGUID guid,
                                                 bool* value) const {
    if (!context || !value)
        return false;

    ITfCompartmentMgr* compartment_mgr = nullptr;
    if (FAILED(context->QueryInterface(IID_ITfCompartmentMgr,
                                       reinterpret_cast<void**>(&compartment_mgr))) ||
        !compartment_mgr) {
        return false;
    }

    ITfCompartment* compartment = nullptr;
    HRESULT hr = compartment_mgr->GetCompartment(guid, &compartment);
    compartment_mgr->Release();
    if (FAILED(hr) || !compartment)
        return false;

    VARIANT current = {};
    VariantInit(&current);
    bool found = false;
    if (SUCCEEDED(compartment->GetValue(&current))) {
        if (current.vt == VT_I4 || current.vt == VT_INT) {
            *value = current.lVal != 0;
            found = true;
        } else if (current.vt == VT_UI4 || current.vt == VT_UINT) {
            *value = current.ulVal != 0;
            found = true;
        } else if (current.vt == VT_BOOL) {
            *value = current.boolVal != VARIANT_FALSE;
            found = true;
        }
    }
    VariantClear(&current);
    compartment->Release();
    return found;
}

bool TextService::_context_keyboard_disabled(ITfContext* context) const {
    if (!context)
        return true;

    bool disabled = false;
    if (_read_context_compartment_bool(context, GUID_COMPARTMENT_KEYBOARD_DISABLED, &disabled) &&
        disabled) {
        return true;
    }

    bool empty_context = false;
    if (_read_context_compartment_bool(context, GUID_COMPARTMENT_EMPTYCONTEXT, &empty_context) &&
        empty_context) {
        return true;
    }

    return false;
}

const char* TextService::_input_context_block_reason(ITfContext* context) const {
    if (!context)
        return "no_context";
    if (!_context_belongs_to_foreground(context))
        return "context_not_foreground";
    if (_context_keyboard_disabled(context))
        return "keyboard_disabled";

    TF_STATUS status = {};
    const HRESULT status_hr = context->GetStatus(&status);
    if (status_hr == TF_E_DISCONNECTED) {
        return "no_context";
    }
    if (FAILED(status_hr)) {
        return nullptr;
    }

    if ((status.dwDynamicFlags & TF_SD_READONLY) != 0)
        return "readonly";

    return nullptr;
}

bool TextService::_context_allows_input(ITfContext* context) const {
    return _input_context_block_reason(context) == nullptr;
}

bool TextService::_document_allows_input(ITfDocumentMgr* doc_mgr) const {
    if (!doc_mgr)
        return false;

    ITfContext* context = nullptr;
    HRESULT hr = doc_mgr->GetTop(&context);
    if (FAILED(hr) || !context)
        return false;

    bool allowed = _context_allows_input(context);
    context->Release();
    return allowed;
}

bool TextService::_query_input_focus_from_thread_mgr() const {
    bool focused = false;
    if (_threadMgr) {
        ITfDocumentMgr* doc_mgr = nullptr;
        if (SUCCEEDED(_threadMgr->GetFocus(&doc_mgr)) && doc_mgr) {
            focused = _document_allows_input(doc_mgr);
            doc_mgr->Release();
        }
    }

    return focused;
}
