// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "reading_ui_element.h"
#include "globals.h"
#include "text_service.h"
#include "tsf_trace.h"
#include <algorithm>

ReadingUIElement::ReadingUIElement(TextService* service)
    : _service(service) {
    if (_service)
        _service->AddRef();
}

ReadingUIElement::~ReadingUIElement() {
    release_context();
    if (_service)
        _service->Release();
}

bool ReadingUIElement::wants_external_window() const {
    if (!_active) {
        return true;
    }
    return _show_external != FALSE;
}

STDMETHODIMP ReadingUIElement::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfUIElement) ||
        IsEqualIID(riid, IID_ITfReadingInformationUIElement)) {
        *ppvObj = static_cast<ITfReadingInformationUIElement*>(this);
    }

    const HRESULT result = *ppvObj ? S_OK : E_NOINTERFACE;
    cxxime_tsf::trace_ui_query(_service, "reading", riid, result);
    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ReadingUIElement::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) ReadingUIElement::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

STDMETHODIMP ReadingUIElement::GetDescription(BSTR* pbstrDescription) {
    if (!pbstrDescription)
        return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"Zhiyi IME Reading");
    return *pbstrDescription ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP ReadingUIElement::GetGUID(GUID* pguid) {
    if (!pguid)
        return E_INVALIDARG;
    *pguid = c_guidReadingUIElement;
    return S_OK;
}

STDMETHODIMP ReadingUIElement::Show(BOOL show) {
    _shown = show;
    cxxime_tsf::trace_ui_show(
        _service, "reading", _ui_element_id, show != FALSE, _shown != FALSE, S_OK);
    return S_OK;
}

STDMETHODIMP ReadingUIElement::IsShown(BOOL* show) {
    if (!show)
        return E_INVALIDARG;
    *show = _shown;
    return S_OK;
}

STDMETHODIMP ReadingUIElement::GetUpdatedFlags(DWORD* flags) {
    if (_service)
        _service->trace_ui_element_method("reading", "GetUpdatedFlags");
    if (!flags)
        return E_INVALIDARG;
    *flags = TF_RIUIE_CONTEXT | TF_RIUIE_STRING | TF_RIUIE_MAXREADINGSTRINGLENGTH |
             TF_RIUIE_ERRORINDEX | TF_RIUIE_VERTICALORDER;
    cxxime_tsf::trace_ui_get_number(
        _service, "reading", _ui_element_id, "GetUpdatedFlags", "updated_flags", *flags);
    return S_OK;
}

STDMETHODIMP ReadingUIElement::GetContext(ITfContext** context) {
    if (_service)
        _service->trace_ui_element_method("reading", "GetContext");
    if (!context)
        return E_INVALIDARG;
    *context = nullptr;
    if (!_context) {
        cxxime_tsf::trace_ui_get_presence(
            _service, "reading", _ui_element_id, "GetContext", "context_present", false,
            E_FAIL);
        return E_FAIL;
    }
    _context->AddRef();
    *context = _context;
    cxxime_tsf::trace_ui_get_presence(
        _service, "reading", _ui_element_id, "GetContext", "context_present", true, S_OK);
    return S_OK;
}

STDMETHODIMP ReadingUIElement::GetString(BSTR* text) {
    if (_service)
        _service->trace_ui_element_method("reading", "GetString", true);
    if (!text)
        return E_INVALIDARG;
    *text = SysAllocStringLen(_reading.c_str(), static_cast<UINT>(_reading.size()));
    const HRESULT result = *text ? S_OK : E_OUTOFMEMORY;
    cxxime_tsf::trace_ui_get_number(
        _service, "reading", _ui_element_id, "GetString", "text_len", _reading.size(), result);
    return result;
}

STDMETHODIMP ReadingUIElement::GetMaxReadingStringLength(UINT* max_length) {
    if (_service)
        _service->trace_ui_element_method("reading", "GetMaxReadingStringLength");
    if (!max_length)
        return E_INVALIDARG;
    *max_length = std::max<UINT>(_max_reading_length, static_cast<UINT>(_reading.size()));
    cxxime_tsf::trace_ui_get_number(_service, "reading", _ui_element_id,
                                          "GetMaxReadingStringLength", "max_length",
                                          *max_length);
    return S_OK;
}

STDMETHODIMP ReadingUIElement::GetErrorIndex(UINT* error_index) {
    if (_service)
        _service->trace_ui_element_method("reading", "GetErrorIndex");
    if (!error_index)
        return E_INVALIDARG;
    *error_index = static_cast<UINT>(-1);
    cxxime_tsf::trace_ui_get_number(
        _service, "reading", _ui_element_id, "GetErrorIndex", "error_index", *error_index);
    return S_OK;
}

STDMETHODIMP ReadingUIElement::IsVerticalOrderPreferred(BOOL* vertical) {
    if (_service)
        _service->trace_ui_element_method("reading", "IsVerticalOrderPreferred");
    if (!vertical)
        return E_INVALIDARG;
    *vertical = FALSE;
    cxxime_tsf::trace_ui_get_bool(
        _service, "reading", _ui_element_id, "IsVerticalOrderPreferred", "vertical", false);
    return S_OK;
}

void ReadingUIElement::set_reading(ITfContext* context, const std::wstring& reading) {
    if (_context != context) {
        release_context();
        _context = context;
        if (_context)
            _context->AddRef();
    }
    _reading = reading;
    _max_reading_length = std::max<UINT>(_max_reading_length, static_cast<UINT>(_reading.size()));
    cxxime_tsf::trace_reading_snapshot(
        _service, _reading, _max_reading_length, _context != nullptr);
}

bool ReadingUIElement::begin(ITfThreadMgr* thread_mgr) {
    if (_active)
        return wants_external_window();

    ITfUIElementMgr* ui_mgr = nullptr;
    if (FAILED(query_ui_element_mgr(thread_mgr, &ui_mgr)) || !ui_mgr) {
        _show_external = TRUE;
        const bool show_external = true;
        cxxime_tsf::trace_reading_lifecycle(
            _service, "begin", TF_INVALID_UIELEMENTID, E_NOINTERFACE,
            "ui_element_mgr_unavailable", &show_external);
        return true;
    }

    _show_external = TRUE;
    DWORD element_id = TF_INVALID_UIELEMENTID;
    HRESULT hr = ui_mgr->BeginUIElement(
        static_cast<ITfReadingInformationUIElement*>(this), &_show_external, &element_id);
    ui_mgr->Release();

    if (FAILED(hr)) {
        _show_external = TRUE;
        const bool show_external = true;
        cxxime_tsf::trace_reading_lifecycle(
            _service, "begin", element_id, hr, "failed", &show_external);
        return true;
    }

    _active = true;
    _ui_element_id = element_id;
    _shown = _show_external;
    const bool show_external = _show_external != FALSE;
    cxxime_tsf::trace_reading_lifecycle(
        _service, "begin", _ui_element_id, hr, "success", &show_external);
    return wants_external_window();
}

void ReadingUIElement::notify_update(ITfThreadMgr* thread_mgr) {
    if (!_active)
        return;

    ITfUIElementMgr* ui_mgr = nullptr;
    if (SUCCEEDED(query_ui_element_mgr(thread_mgr, &ui_mgr)) && ui_mgr) {
        const HRESULT hr = ui_mgr->UpdateUIElement(_ui_element_id);
        cxxime_tsf::trace_reading_lifecycle(
            _service, "update", _ui_element_id, hr, SUCCEEDED(hr) ? "success" : "failed");
        ui_mgr->Release();
    } else {
        cxxime_tsf::trace_reading_lifecycle(
            _service, "update", _ui_element_id, E_NOINTERFACE, "ui_element_mgr_unavailable");
    }
}

void ReadingUIElement::end(ITfThreadMgr* thread_mgr) {
    if (!_active) {
        _reading.clear();
        release_context();
        return;
    }

    ITfUIElementMgr* ui_mgr = nullptr;
    HRESULT end_hr = E_NOINTERFACE;
    if (SUCCEEDED(query_ui_element_mgr(thread_mgr, &ui_mgr)) && ui_mgr) {
        end_hr = ui_mgr->EndUIElement(_ui_element_id);
        ui_mgr->Release();
    }

    cxxime_tsf::trace_reading_lifecycle(
        _service, "end", _ui_element_id, end_hr, SUCCEEDED(end_hr) ? "success" : "failed");

    _active = false;
    _show_external = TRUE;
    _shown = FALSE;
    _ui_element_id = TF_INVALID_UIELEMENTID;
    _reading.clear();
    release_context();
}

HRESULT ReadingUIElement::query_ui_element_mgr(ITfThreadMgr* thread_mgr,
                                               ITfUIElementMgr** ui_mgr) const {
    if (!ui_mgr)
        return E_INVALIDARG;
    *ui_mgr = nullptr;
    if (!thread_mgr)
        return E_FAIL;
    return thread_mgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(ui_mgr));
}

void ReadingUIElement::release_context() {
    if (_context) {
        _context->Release();
        _context = nullptr;
    }
}
