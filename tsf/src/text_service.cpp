// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>

#include <cxxime/candidate_window.h>
#include <cxxime/logging.h>

#include "candidate_ui_element.h"
#include "config_coordinator.h"
#include "globals.h"
#include "reading_ui_element.h"
#include "search_candidate_list.h"
#include "tsf_activation.h"
#include "tsf_trace.h"

namespace {

std::string wstring_to_utf8(const wchar_t* text) {
    if (!text || *text == L'\0') {
        return {};
    }
    const int source_length = static_cast<int>(wcslen(text));
    const int byte_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, source_length, nullptr, 0, nullptr, nullptr);
    if (byte_length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(byte_length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, source_length,
                            result.data(), byte_length, nullptr, nullptr) != byte_length) {
        return {};
    }
    return result;
}

} // namespace

TextService::TextService() {
    DllAddRef();
}

TextService::~TextService() {
    // TSF normally calls Deactivate before releasing the service. Keep destruction safe for
    // hosts that tear down an instance directly after a partial activation.
    Deactivate();
    _localCandidateWindow.reset();
    DllRelease();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor))
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
        *ppvObj = static_cast<ITfThreadFocusSink*>(this);
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    else if (IsEqualIID(riid, IID_ITfCompartmentEventSink))
        *ppvObj = static_cast<ITfCompartmentEventSink*>(this);
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
        *ppvObj = static_cast<ITfTextEditSink*>(this);
    else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
        *ppvObj = static_cast<ITfTextLayoutSink*>(this);
    else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    else if (IsEqualIID(riid, IID_ITfFunction))
        *ppvObj = static_cast<ITfFunction*>(this);
    else if (IsEqualIID(riid, IID_ITfFunctionProvider))
        *ppvObj = static_cast<ITfFunctionProvider*>(this);
    else if (IsEqualIID(riid, IID_ITfFnSearchCandidateProvider))
        *ppvObj = static_cast<ITfFnSearchCandidateProvider*>(this);

    const bool function_interface = IsEqualIID(riid, IID_ITfFunction) ||
                                    IsEqualIID(riid, IID_ITfFunctionProvider) ||
                                    IsEqualIID(riid, IID_ITfFnSearchCandidateProvider);
    const HRESULT result = *ppvObj ? S_OK : E_NOINTERFACE;
    if (function_interface) {
        cxxime_tsf::trace_ui_query(this, "function_provider", riid, result);
    }
    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) TextService::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

STDMETHODIMP TextService::GetType(GUID* guid) {
    if (!guid) {
        return E_INVALIDARG;
    }
    *guid = c_clsidTextService;
    return S_OK;
}

STDMETHODIMP TextService::GetDescription(BSTR* description) {
    if (!description) {
        return E_INVALIDARG;
    }
    *description = SysAllocString(TEXTSERVICE_DESC);
    return *description ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetFunction(REFGUID function_guid,
                                      REFIID riid,
                                      IUnknown** function) {
    if (!function) {
        return E_INVALIDARG;
    }
    *function = nullptr;
    if (!IsEqualGUID(function_guid, GUID_NULL)) {
        return E_NOINTERFACE;
    }
    return QueryInterface(riid, reinterpret_cast<void**>(function));
}

STDMETHODIMP TextService::GetDisplayName(BSTR* name) {
    if (!name) {
        return E_INVALIDARG;
    }
    *name = SysAllocString(L"Zhiyi IME Search Candidates");
    return *name ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetSearchCandidates(BSTR query,
                                              BSTR application_id,
                                              ITfCandidateList** candidates) {
    UNREFERENCED_PARAMETER(application_id);
    if (!candidates) {
        return E_INVALIDARG;
    }
    *candidates = nullptr;
    const std::string query_utf8 = wstring_to_utf8(query);
    if (query && *query != L'\0' && query_utf8.empty()) {
        return E_INVALIDARG;
    }

    cxxime::IPCResponse response = {};
    std::vector<std::wstring> values;
    if (!query_utf8.empty() && _client.search_candidates(query_utf8, response)) {
        for (uint32_t index = 0; index < response.candidate_count &&
                                index < cxxime::kCandidateCapacity; ++index) {
            values.push_back(utf8_to_wstring(response.candidates[index]));
        }
    }

    auto* result = new (std::nothrow) SearchCandidateList(std::move(values));
    if (!result) {
        return E_OUTOFMEMORY;
    }
    *candidates = result;
    return S_OK;
}

STDMETHODIMP TextService::SetResult(BSTR query, BSTR application_id, BSTR result) {
    UNREFERENCED_PARAMETER(application_id);
    const std::string query_utf8 = wstring_to_utf8(query);
    const std::string result_utf8 = wstring_to_utf8(result);
    if ((query && *query != L'\0' && query_utf8.empty()) ||
        (result && *result != L'\0' && result_utf8.empty())) {
        return E_INVALIDARG;
    }
    return _client.set_search_result(query_utf8, result_utf8) ? S_OK : E_FAIL;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    OutputDebugStringA("[ZhiyiIME] ActivateEx called\n");
    CXXIME_LOG(L"ActivateEx: clientId=%u, flags=%u", tid, dwFlags);
    {
        char detail[64] = {};
        snprintf(detail, sizeof(detail), "flags=0x%08x", static_cast<unsigned int>(dwFlags));
        _enqueue_event_trace("activate", detail, true);
    }

    _threadMgr = ptim;
    _threadMgr->AddRef();
    _clientId = tid;
    _activateFlags = dwFlags;

    const HRESULT activation_sinks_hr = _initialize_required_activation_sinks();
    if (FAILED(activation_sinks_hr)) {
        cxxime_tsf::shutdown_tsf_log_writer_if_no_config_subscribers();
        return activation_sinks_hr;
    }

    cxxime_tsf::trace_activation_step("runtime_snapshot", "attempt", S_OK, false);
    cxxime_tsf::trace_runtime_activate(dwFlags, tid);
    cxxime_tsf::trace_activation_step("runtime_snapshot", "complete", S_OK, false);

    cxxime_tsf::trace_activation_step("config_updates", "attempt", S_OK, false);
    const HRESULT config_updates_hr = _start_config_updates() ? S_OK : E_FAIL;
    cxxime_tsf::trace_activation_step(
        "config_updates", "complete", config_updates_hr, false);
    if ((_activateFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0) {
        _start_host_compatibility_runtime();
    }
    _initialize_optional_activation_services();

    _candidateUiElement = new (std::nothrow) CandidateUIElement(this);
    _readingUiElement = new (std::nothrow) ReadingUIElement(this);

    // Connect to server and query initial status before registering the input indicator.
    // Pre-set the indicator to match the server before AddItem, so TSF reads the
    // correct icon on the first GetIcon call.
    bool initial_input_allows_input = _query_input_focus_from_thread_mgr();
    cxxime::ImeStatus initial_status = {};
    initial_status.set_chinese_mode(true); // Fallback input-indicator default.
    bool has_last_status = false;
    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        has_last_status = _has_synced_ime_status();
        if (has_last_status) {
            initial_status = _lastImeStatus;
        }
    }
    bool initial_status_available = has_last_status;
    bool initial_caps_lock = initial_input_allows_input && _is_caps_lock_on();
    _sessionId = 0;
    if (_ensure_ipc_session()) {
        if (initial_input_allows_input) {
            if (_sync_caps_lock_state(initial_caps_lock, "activate_focused", &initial_status)) {
                initial_status_available = true;
            }
        }
        cxxime::IPCResponse status_resp = {};
        if (_ensure_ipc_session() &&
            _client.get_status(_sessionId, status_resp) && status_resp.status == cxxime::IPCStatus::OK) {
            initial_status = status_resp.ime_status;
            initial_status_available = true;
        }
    }
    if (initial_input_allows_input && initial_caps_lock) {
        initial_status.set_caps_lock(true);
        auto caps_it = _config.ascii_switch_key.find("Caps_Lock");
        if (caps_it != _config.ascii_switch_key.end() && caps_it->second != "noop") {
            initial_status.set_chinese_mode(false);
        }
    }

    // Keep fallback values for internal state, but do not publish them as a server-synchronized
    // status. The default input mode is Pinyin and must not be shown while startup IPC is pending.
    _chinese_mode = initial_status.chinese_mode();
    _caps_lock = initial_status.caps_lock();
    if (initial_status_available) {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        _lastImeStatus = initial_status;
        _hasLastImeStatus.store(true, std::memory_order_release);
    }

    if (!_inputIndicator.initialize(
            _threadMgr, tid, initial_status,
            [this]() {
                CXXIME_LOG(L"toggle_chinese: sessionId=%u", _sessionId);
                cxxime::IPCResponse resp = {};
                if (_ensure_ipc_session()) {
                    _client.toggle_chinese(_sessionId, resp);
                }
                CXXIME_LOG(L"toggle_chinese: result status=%d, chinese=%d",
                           static_cast<int>(resp.status), resp.ime_status.chinese_mode());
                if (resp.status == cxxime::IPCStatus::OK) {
                    _sync_ime_status(resp.ime_status);
                }
            },
            [this](cxxime::ImeMenuCommand command) { _handle_ime_menu_command(command); },
            _config.status_window.enable)) {
        CXXIME_LOG(L"input_indicator event=initialize result=degraded");
    }

    // Avoid a redundant get_status IPC here. initial_status was already read
    // before language bar registration, so a second sync can block the UI
    // thread and trigger an extra icon refresh.
    // cxxime::IPCResponse resp = {};
    // if (_client.get_status(_sessionId, resp) && resp.status == cxxime::IPCStatus::OK) {
    //     _sync_ime_status(resp.ime_status);
    // }
    _activated = true;
    _start_ui_presentation_channel();
    _synchronize_activation_focus();
    _publish_ui_presentation();
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    CXXIME_LOG(L"Deactivate: sessionId=%u", _sessionId);
    invalidate_composition_edit_requests();
    _activated = false;
    _inputFocused = false;
    _inputTargetUnavailable = false;
    _stop_state_poll_timer();
    _unregister_conversion_compartment_sink();

    if (_sessionId && _composing) {
        cxxime::IPCResponse response = {};
        if (_client.commit_composition(_sessionId, response)) {
            ITfContext* context = _current_edit_context_for_composition();
            BOOL eaten = FALSE;
            _apply_engine_response(context, response, &eaten);
            if (context) {
                context->Release();
            }
        }
    }
    if (_composition) {
        ITfComposition* composition = _composition;
        ITfContext* context = _current_edit_context_for_composition();
        if (context) {
            const HRESULT result = _end_composition(context, true);
            if (FAILED(result)) {
                CXXIME_LOG(L"Deactivate local composition cleanup failed: hr=0x%08x", result);
            }
            context->Release();
        }
        if (_composition == composition) {
            _composition = nullptr;
            composition->Release();
        }
    }
    _composing = false;
    _emptyCompositionPlaceholderActive = false;
    clear_applied_inline_composition_text();

    _hide_status_window("hide:deactivate");
    _hide_candidate_window("hide:deactivate_candidates");
    _publish_ui_presentation();
    _stop_ui_presentation_channel();

    if (_sessionId) {
        _client.end_session(_sessionId);
        _sessionId = 0;
    }
    _lastIpcHeartbeat = {};
    _ipcHealthy = true;
    // Keep the pipe connection for reuse. The next ActivateEx will create a
    // fresh server session and reconnect if the pipe has been closed.

    _stop_config_updates();

    _end_reading_ui_element("hide:deactivate_reading");
    _unadvise_text_edit_sink();
    _unadvise_text_layout_sink();
    _release_effective_edit_target();
    set_composition_context(nullptr);
    if (_candidateUiElement) {
        _candidateUiElement->Release();
        _candidateUiElement = nullptr;
    }
    if (_readingUiElement) {
        _readingUiElement->Release();
        _readingUiElement = nullptr;
    }
    _inputIndicator.shutdown();

    _unregister_thread_sinks();

    _unregister_key_event_sink();
    _unregister_preserved_key();

    if (_threadMgr) {
        _threadMgr->Release();
        _threadMgr = nullptr;
    }
    _clientId = TF_CLIENTID_NULL;
    if ((_activateFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0) {
        _stop_host_compatibility_runtime();
    }

    return S_OK;
}
