// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "candidate_ui_element.h"

#include <algorithm>

#include "globals.h"
#include "text_service.h"
#include "tsf_trace.h"

namespace {

std::wstring utf8_to_wstring(const std::string& text) {
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], len);
    return result;
}

} // namespace

CandidateUIElement::CandidateUIElement(TextService* service)
    : _service(service) {
    if (_service)
        _service->AddRef();
}

CandidateUIElement::~CandidateUIElement() {
    release_document_mgr();
    if (_service)
        _service->Release();
}

STDMETHODIMP CandidateUIElement::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior)) {
        *ppvObj = static_cast<ITfCandidateListUIElementBehavior*>(this);
    } else if (IsEqualIID(riid, IID_ITfIntegratableCandidateListUIElement)) {
        *ppvObj = static_cast<ITfIntegratableCandidateListUIElement*>(this);
    }

    const HRESULT result = *ppvObj ? S_OK : E_NOINTERFACE;
    cxxime_tsf::trace_ui_query(_service, "candidate", riid, result);
    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CandidateUIElement::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) CandidateUIElement::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

STDMETHODIMP CandidateUIElement::GetDescription(BSTR* pbstrDescription) {
    if (!pbstrDescription)
        return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"Zhiyi IME Candidate List");
    return *pbstrDescription ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateUIElement::GetGUID(GUID* pguid) {
    if (!pguid) {
        return E_INVALIDARG;
    }
    *pguid = c_guidCandidateUIElement;
    return S_OK;
}

STDMETHODIMP CandidateUIElement::Show(BOOL show) {
    const bool requested_show = show != FALSE;
    if (!_active || !_service) {
        cxxime_tsf::trace_ui_show(
            _service, "candidate", _ui_element_id, requested_show, false, E_FAIL);
        return E_FAIL;
    }

    const bool accepted = _service->set_candidate_ui_element_shown(requested_show);
    if (accepted) {
        _show_external = show;
    }
    const bool actual_show = _service->is_candidate_ui_element_shown();
    _shown = actual_show ? TRUE : FALSE;
    const HRESULT result = accepted ? S_OK : E_FAIL;
    cxxime_tsf::trace_ui_show(
        _service, "candidate", _ui_element_id, requested_show, actual_show, result);
    return result;
}

bool CandidateUIElement::wants_external_window() const {
    if (!_active) {
        return true;
    }
    return _show_external != FALSE;
}

STDMETHODIMP CandidateUIElement::IsShown(BOOL* show) {
    if (!show) {
        return E_INVALIDARG;
    }
    _shown = _active && _service && _service->is_candidate_ui_element_shown() ? TRUE : FALSE;
    *show = _shown;
    cxxime_tsf::trace_ui_get_bool(
        _service, "candidate", _ui_element_id, "IsShown", "shown", _shown != FALSE);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetUpdatedFlags(DWORD* flags) {
    if (_service) {
        _service->trace_ui_element_method("candidate", "GetUpdatedFlags");
    }
    if (!flags) {
        return E_INVALIDARG;
    }
    *flags = kPublishedUpdatedFlags;
    cxxime_tsf::trace_ui_get_number(
        _service, "candidate", _ui_element_id, "GetUpdatedFlags", "updated_flags", *flags);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetDocumentMgr(ITfDocumentMgr** doc_mgr) {
    if (_service) {
        _service->trace_ui_element_method("candidate", "GetDocumentMgr");
    }
    if (!doc_mgr) {
        return E_INVALIDARG;
    }
    *doc_mgr = nullptr;
    if (!_document_mgr) {
        cxxime_tsf::trace_ui_get_presence(
            _service, "candidate", _ui_element_id, "GetDocumentMgr",
            "document_mgr_present", false, E_FAIL);
        return E_FAIL;
    }
    _document_mgr->AddRef();
    *doc_mgr = _document_mgr;
    cxxime_tsf::trace_ui_get_presence(
        _service, "candidate", _ui_element_id, "GetDocumentMgr", "document_mgr_present",
        true, S_OK);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetCount(UINT* count) {
    if (_service)
        _service->trace_ui_element_method("candidate", "GetCount", true);
    if (!count)
        return E_INVALIDARG;
    *count = static_cast<UINT>(_candidates.size());
    cxxime_tsf::trace_ui_get_number(
        _service, "candidate", _ui_element_id, "GetCount", "count", *count);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetSelection(UINT* index) {
    if (_service)
        _service->trace_ui_element_method("candidate", "GetSelection");
    if (!index)
        return E_INVALIDARG;
    *index = _selection;
    cxxime_tsf::trace_ui_get_number(
        _service, "candidate", _ui_element_id, "GetSelection", "selection", *index);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetString(UINT index, BSTR* text) {
    if (_service)
        _service->trace_ui_element_method("candidate", "GetString", true);
    if (!text)
        return E_INVALIDARG;
    *text = nullptr;
    if (index >= _candidates.size()) {
        cxxime_tsf::trace_candidate_get_string(
            _service, _ui_element_id, index, nullptr, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const auto& candidate = _candidates[index];
    *text = SysAllocStringLen(candidate.c_str(), static_cast<UINT>(candidate.size()));
    const HRESULT result = *text ? S_OK : E_OUTOFMEMORY;
    cxxime_tsf::trace_candidate_get_string(
        _service, _ui_element_id, index, &candidate, result);
    return result;
}

STDMETHODIMP CandidateUIElement::GetPageIndex(UINT* index, UINT size, UINT* page_count) {
    if (_service)
        _service->trace_ui_element_method("candidate", "GetPageIndex");
    if (!page_count)
        return E_INVALIDARG;
    *page_count = 1;
    if (!index) {
        cxxime_tsf::trace_candidate_get_page(
            _service, _ui_element_id, size, *page_count, true, 0, S_OK);
        return S_OK;
    }
    if (size < 1)
        return E_INVALIDARG;
    index[0] = 0;
    cxxime_tsf::trace_candidate_get_page(
        _service, _ui_element_id, size, *page_count, false, index[0], S_OK);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::SetPageIndex(UINT* index, UINT page_count) {
    if (!index || page_count != 1 || index[0] != 0)
        return E_INVALIDARG;
    cxxime_tsf::trace_candidate_page_set(
        _service, _ui_element_id, page_count, index[0], S_OK);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::GetCurrentPage(UINT* page) {
    if (_service)
        _service->trace_ui_element_method("candidate", "GetCurrentPage");
    if (!page)
        return E_INVALIDARG;
    *page = 0;
    cxxime_tsf::trace_ui_get_number(
        _service, "candidate", _ui_element_id, "GetCurrentPage", "current_page", *page);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::SetSelection(UINT index) {
    if (index >= _candidates.size())
        return E_INVALIDARG;
    _selection = index;
    cxxime_tsf::trace_candidate_behavior_number(
        _service, _ui_element_id, "SetSelection", "selection", index);
    return S_OK;
}

STDMETHODIMP CandidateUIElement::Finalize() {
    const DWORD element_id = _ui_element_id;
    const UINT selection = _selection;
    HRESULT result = E_FAIL;
    if (_service && !_candidates.empty()) {
        result = _service->select_candidate_from_ui(selection, _candidateRevision) ? S_OK
            : E_FAIL;
    }
    cxxime_tsf::trace_candidate_behavior_number(
        _service, element_id, "Finalize", "selection", selection, result);
    return result;
}

STDMETHODIMP CandidateUIElement::Abort() {
    if (_service)
        _service->abort_candidate_ui_from_tsf();
    return S_OK;
}

STDMETHODIMP CandidateUIElement::SetIntegrationStyle(GUID guidIntegrationStyle) {
    if (_service) {
        _service->trace_ui_element_method("candidate", "SetIntegrationStyle");
    }
    return IsEqualGUID(guidIntegrationStyle, GUID_INTEGRATIONSTYLE_SEARCHBOX)
        ? S_OK
        : E_NOTIMPL;
}

STDMETHODIMP
CandidateUIElement::GetSelectionStyle(TfIntegratableCandidateListSelectionStyle* selection_style) {
    if (!selection_style) {
        return E_INVALIDARG;
    }
    *selection_style = STYLE_ACTIVE_SELECTION;
    if (_service) {
        _service->trace_ui_element_method("candidate", "GetSelectionStyle");
    }
    return S_OK;
}

STDMETHODIMP CandidateUIElement::OnKeyDown(WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (!eaten) {
        return E_INVALIDARG;
    }
    // Integrated hosts own the rendered candidate surface, so forwarded keys
    // are consumed here after the normal key sink has handled physical input.
    *eaten = TRUE;
    if (_service) {
        _service->trace_ui_element_method("candidate", "OnKeyDown");
    }
    return S_OK;
}

STDMETHODIMP CandidateUIElement::ShowCandidateNumbers(BOOL* show) {
    if (!show) {
        return E_INVALIDARG;
    }
    *show = TRUE;
    if (_service) {
        _service->trace_ui_element_method("candidate", "ShowCandidateNumbers");
    }
    return S_OK;
}

STDMETHODIMP CandidateUIElement::FinalizeExactCompositionString() {
    if (!_service) {
        return E_FAIL;
    }
    return _service->finalize_exact_candidate_ui_from_tsf();
}

void CandidateUIElement::set_page(const cxxime::CandidatePresentationPage& page,
                                  std::uint64_t candidate_revision,
                                  int page_current,
                                  int page_total) {
    _candidates.clear();
    _candidates.reserve(page.items.size());
    for (const auto& candidate : page.items) {
        _candidates.push_back(utf8_to_wstring(
            cxxime::format_candidate_presentation(candidate)));
    }
    _candidateRevision = candidate_revision;

    if (_candidates.empty()) {
        _selection = 0;
    } else if (page.highlighted >= 0 &&
               static_cast<size_t>(page.highlighted) < _candidates.size()) {
        _selection = static_cast<UINT>(page.highlighted);
    } else {
        _selection = std::min<UINT>(_selection, static_cast<UINT>(_candidates.size() - 1));
    }
    cxxime_tsf::trace_candidate_snapshot(
        _service, _candidates, _selection, page_current, page_total);
}

void CandidateUIElement::clear_page() {
    _candidates.clear();
    _candidateRevision = 0;
    _selection = 0;
}

bool CandidateUIElement::begin(ITfThreadMgr* thread_mgr, ITfDocumentMgr* document_mgr) {
    if (_active) {
        return wants_external_window();
    }

    ITfUIElementMgr* ui_mgr = nullptr;
    if (FAILED(query_ui_element_mgr(thread_mgr, &ui_mgr)) || !ui_mgr) {
        _show_external = TRUE;
        const bool show_external = true;
        cxxime_tsf::trace_candidate_lifecycle(
            _service, "begin", TF_INVALID_UIELEMENTID, E_NOINTERFACE,
            "ui_element_mgr_unavailable", &show_external);
        return true;
    }

    capture_document_mgr(thread_mgr, document_mgr);
    if (_service) {
        _service->trace_candidate_activation_state(_document_mgr);
    }
    _show_external = TRUE;
    DWORD element_id = TF_INVALID_UIELEMENTID;
    HRESULT hr = ui_mgr->BeginUIElement(
        static_cast<ITfCandidateListUIElementBehavior*>(this), &_show_external, &element_id);
    ui_mgr->Release();

    if (FAILED(hr)) {
        release_document_mgr();
        _show_external = TRUE;
        const bool show_external = true;
        cxxime_tsf::trace_candidate_lifecycle(
            _service, "begin", element_id, hr, "failed", &show_external);
        return true;
    }

    _active = true;
    _ui_element_id = element_id;
    _shown = FALSE;
    const bool show_external = _show_external != FALSE;
    cxxime_tsf::trace_candidate_lifecycle(
        _service, "begin", _ui_element_id, hr, "success", &show_external);
    cxxime_tsf::trace_external_ui_decision(_service, _ui_element_id, show_external);
    return wants_external_window();
}

void CandidateUIElement::notify_update(ITfThreadMgr* thread_mgr) {
    if (!_active)
        return;
    ITfUIElementMgr* ui_mgr = nullptr;
    if (SUCCEEDED(query_ui_element_mgr(thread_mgr, &ui_mgr)) && ui_mgr) {
        const HRESULT hr = ui_mgr->UpdateUIElement(_ui_element_id);
        cxxime_tsf::trace_candidate_lifecycle(
            _service, "update", _ui_element_id, hr, SUCCEEDED(hr) ? "success" : "failed");
        ui_mgr->Release();
    } else {
        cxxime_tsf::trace_candidate_lifecycle(
            _service, "update", _ui_element_id, E_NOINTERFACE, "ui_element_mgr_unavailable");
    }
}

void CandidateUIElement::end(ITfThreadMgr* thread_mgr) {
    clear_page();
    if (!_active)
        return;

    ITfUIElementMgr* ui_mgr = nullptr;
    HRESULT end_hr = E_NOINTERFACE;
    if (SUCCEEDED(query_ui_element_mgr(thread_mgr, &ui_mgr)) && ui_mgr) {
        end_hr = ui_mgr->EndUIElement(_ui_element_id);
        ui_mgr->Release();
    }

    cxxime_tsf::trace_candidate_lifecycle(
        _service, "end", _ui_element_id, end_hr, SUCCEEDED(end_hr) ? "success" : "failed");

    _active = false;
    _show_external = TRUE;
    _shown = FALSE;
    _ui_element_id = TF_INVALID_UIELEMENTID;
    release_document_mgr();
}

HRESULT CandidateUIElement::query_ui_element_mgr(ITfThreadMgr* thread_mgr,
                                                  ITfUIElementMgr** ui_mgr) const {
    if (!ui_mgr)
        return E_INVALIDARG;
    *ui_mgr = nullptr;
    if (!thread_mgr)
        return E_FAIL;
    return thread_mgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(ui_mgr));
}

void CandidateUIElement::capture_document_mgr(ITfThreadMgr* thread_mgr,
                                              ITfDocumentMgr* document_mgr) {
    ITfDocumentMgr* doc_mgr = document_mgr;
    if (doc_mgr) {
        doc_mgr->AddRef();
    } else if (!thread_mgr || FAILED(thread_mgr->GetFocus(&doc_mgr)) || !doc_mgr) {
        return;
    }

    if (_document_mgr != doc_mgr) {
        release_document_mgr();
        _document_mgr = doc_mgr;
    } else {
        doc_mgr->Release();
    }
}

void CandidateUIElement::release_document_mgr() {
    if (_document_mgr) {
        _document_mgr->Release();
        _document_mgr = nullptr;
    }
}
