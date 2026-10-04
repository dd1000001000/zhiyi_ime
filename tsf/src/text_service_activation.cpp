// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <new>

#include <cxxime/logging.h>
#include <cxxime/tsf_factory.h>

#include "about_dialog.h"
#include "config_coordinator.h"
#include "display_attribute.h"
#include "globals.h"
#include "host_compatibility/host_classification_compatibility.h"
#include "tsf_activation.h"
#include "tsf_imm_mode.h"
#include "tsf_ui_element_observer.h"

void TextService::_handle_ime_menu_command(cxxime::ImeMenuCommand command) {
    if (command == cxxime::ImeMenuCommand::kPinyin ||
        command == cxxime::ImeMenuCommand::kWubi ||
        command == cxxime::ImeMenuCommand::kMixed) {
        cxxime::InputMode mode = cxxime::InputMode::PINYIN;
        if (command == cxxime::ImeMenuCommand::kWubi) {
            mode = cxxime::InputMode::WUBI;
        } else if (command == cxxime::ImeMenuCommand::kMixed) {
            mode = cxxime::InputMode::MIXED;
        }

        CXXIME_LOG(L"menu_command: input_mode=%d, sessionId=%u",
                   static_cast<int>(mode), _sessionId);
        cxxime::IPCResponse response = {};
        if (_ensure_ipc_session()) {
            _client.switch_input_mode(_sessionId, mode, response);
        }
        if (response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
        return;
    }

    switch (command) {
    case cxxime::ImeMenuCommand::kDictionary:
        if (!_ensure_ipc_session() ||
            !_client.open_settings(_sessionId, cxxime::SettingsPanel::kDictionary)) {
            CXXIME_LOG(L"%s", L"settings_request source=tsf panel=dictionary result=0");
        }
        break;
    case cxxime::ImeMenuCommand::kToggleStatusWindow: {
        bool enabled = !_config.status_window.enable;
        cxxime_tsf::set_status_window_enabled(enabled);
        break;
    }
    case cxxime::ImeMenuCommand::kSettings:
        if (!_ensure_ipc_session() ||
            !_client.open_settings(_sessionId, cxxime::SettingsPanel::kInput)) {
            CXXIME_LOG(L"%s", L"settings_request source=tsf panel=input result=0");
        }
        break;
    case cxxime::ImeMenuCommand::kAbout:
        show_about_dialog();
        break;
    case cxxime::ImeMenuCommand::kPinyin:
    case cxxime::ImeMenuCommand::kWubi:
    case cxxime::ImeMenuCommand::kMixed:
        break;
    }
}

void TextService::_start_host_compatibility_runtime() {
    if (_hostCompatibilityRuntimeActive) {
        return;
    }

    cxxime_tsf::activate_host_classification_compatibility();
    cxxime_tsf::start_host_trace_runtime(
        cxxime_tsf::host_classification_compatibility_snapshot());
    _hostCompatibilityRuntimeActive = true;
}

void TextService::_stop_host_compatibility_runtime() {
    if (!_hostCompatibilityRuntimeActive) {
        return;
    }

    const cxxime_tsf::HostClassificationCompatibilitySnapshot snapshot =
        cxxime_tsf::deactivate_host_classification_compatibility();
    cxxime_tsf::stop_host_trace_runtime(snapshot);
    _hostCompatibilityRuntimeActive = false;
}

void TextService::_sync_conversion_mode_compartment(
    const cxxime::ImeStatus& status) {
    if (!_threadMgr || _clientId == TF_CLIENTID_NULL) {
        return;
    }

    ITfCompartmentMgr* compartment_mgr = nullptr;
    HRESULT hr = _threadMgr->QueryInterface(
        IID_ITfCompartmentMgr, reinterpret_cast<void**>(&compartment_mgr));
    if (FAILED(hr) || !compartment_mgr) {
        return;
    }

    ITfCompartment* compartment = nullptr;
    hr = compartment_mgr->GetCompartment(
        GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &compartment);
    compartment_mgr->Release();
    if (FAILED(hr) || !compartment) {
        return;
    }

    DWORD conversion_mode = 0;
    VARIANT current = {};
    VariantInit(&current);
    const HRESULT get_value_hr = compartment->GetValue(&current);
    if (SUCCEEDED(get_value_hr)) {
        if (current.vt == VT_I4 || current.vt == VT_INT) {
            conversion_mode = static_cast<DWORD>(current.lVal);
        } else if (current.vt == VT_UI4 || current.vt == VT_UINT) {
            conversion_mode = current.ulVal;
        }
    }
    VariantClear(&current);

    constexpr DWORD kChineseMode =
        TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_SYMBOL;
    DWORD requested_mode = conversion_mode;
    if (status.chinese_mode()) {
        requested_mode |= kChineseMode;
    } else {
        requested_mode &= ~kChineseMode;
    }

    const bool set_attempted = requested_mode != conversion_mode;
    HRESULT set_value_hr = S_FALSE;
    if (set_attempted) {
        VARIANT next = {};
        VariantInit(&next);
        next.vt = VT_I4;
        next.lVal = static_cast<LONG>(requested_mode);
        _writingConversionCompartment = true;
        set_value_hr = compartment->SetValue(_clientId, &next);
        _writingConversionCompartment = false;
        CXXIME_LOG(L"sync_conversion_mode: chinese=%d, mode=0x%08x->0x%08x, hr=0x%08x",
                   status.chinese_mode() ? 1 : 0, conversion_mode,
                   requested_mode, set_value_hr);
        VariantClear(&next);
    }
    cxxime_tsf::trace_conversion_compartment(
        status.chinese_mode(), get_value_hr, conversion_mode, requested_mode,
        set_attempted, set_value_hr);
    compartment->Release();
}

HRESULT TextService::_initialize_required_activation_sinks() {
    cxxime_tsf::trace_activation_step("activate", "begin", S_OK, true);

    const auto fail_activation = [this](HRESULT result, bool key_sink_registered) {
        if (key_sink_registered) {
            _unregister_key_event_sink();
        }
        _unregister_thread_sinks();
        _threadMgr->Release();
        _threadMgr = nullptr;
        _clientId = TF_CLIENTID_NULL;
        cxxime_tsf::trace_activation_step("activate", "failed", result, true);
        return result;
    };

    cxxime_tsf::trace_activation_step("thread_mgr_event_sink", "attempt", S_OK, true);
    const HRESULT thread_mgr_hr = _register_thread_mgr_event_sink();
    cxxime_tsf::trace_activation_step("thread_mgr_event_sink", "complete", thread_mgr_hr,
                                            true);
    if (FAILED(thread_mgr_hr)) {
        return fail_activation(thread_mgr_hr, false);
    }

    cxxime_tsf::trace_activation_step("key_event_sink", "attempt", S_OK, true);
    const HRESULT key_event_sink_hr = _register_key_event_sink();
    cxxime_tsf::trace_activation_step("key_event_sink", "complete", key_event_sink_hr, true);
    if (FAILED(key_event_sink_hr)) {
        return fail_activation(key_event_sink_hr, false);
    }

    cxxime_tsf::trace_activation_step("thread_focus_sink", "attempt", S_OK, true);
    const HRESULT thread_focus_hr = _register_thread_focus_sink();
    cxxime_tsf::trace_activation_step("thread_focus_sink", "complete", thread_focus_hr, true);
    cxxime_tsf::trace_thread_sinks("advise", S_OK, true, thread_focus_hr,
                                         _dwThreadFocusCookie, true, thread_mgr_hr,
                                         _dwThreadMgrEventCookie);
    if (FAILED(thread_focus_hr)) {
        return fail_activation(thread_focus_hr, true);
    }

    return S_OK;
}

void TextService::_initialize_optional_activation_services() {
    cxxime_tsf::trace_activation_step("ui_element_observer", "attempt", S_OK, false);
    cxxime_tsf::start_ui_element_observer(_threadMgr, _activateFlags);
    cxxime_tsf::trace_activation_step("ui_element_observer", "complete", S_OK, false);

    cxxime_tsf::trace_activation_step("display_attribute", "attempt", S_OK, false);
    const HRESULT display_attribute_hr = _register_display_attribute_atom() ? S_OK : E_FAIL;
    cxxime_tsf::trace_activation_step("display_attribute", "complete", display_attribute_hr,
                                            false);


    cxxime_tsf::trace_activation_step("conversion_sink", "attempt", S_OK, false);
    _register_conversion_compartment_sink();
    cxxime_tsf::trace_activation_step("conversion_sink", "complete", S_OK, false);
}

void TextService::_synchronize_activation_focus() {
    if (_synchronize_effective_edit_target_from_thread_mgr("activate_complete")) {
        _refresh_caps_lock_on_focus("activate_complete");
        _schedule_caps_lock_refresh();
        if (_sessionId && _client.ensure_connected()) {
            _client.focus_in(_sessionId);
        }
    }

    if (_config.status_window.enable && _config.status_window.show_on_startup && _inputFocused) {
        _show_status_window_if_allowed("show:activate_startup");
    }
    cxxime_tsf::trace_activation_step("activate", "complete", S_OK, true);
}

HRESULT TextService::_register_thread_mgr_event_sink() {
    ITfSource* source = nullptr;
    const HRESULT source_hr =
        _threadMgr ? _threadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))
                   : E_POINTER;
    if (FAILED(source_hr) || !source) {
        return FAILED(source_hr) ? source_hr : E_NOINTERFACE;
    }

    const HRESULT result =
        source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
                           &_dwThreadMgrEventCookie);
    source->Release();
    return result;
}

HRESULT TextService::_register_thread_focus_sink() {
    ITfSource* source = nullptr;
    const HRESULT source_hr =
        _threadMgr ? _threadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))
                   : E_POINTER;
    if (FAILED(source_hr) || !source) {
        return FAILED(source_hr) ? source_hr : E_NOINTERFACE;
    }

    const HRESULT result = source->AdviseSink(
        IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this), &_dwThreadFocusCookie);
    source->Release();
    return result;
}

void TextService::_unregister_thread_sinks() {
    if (!_threadMgr) {
        _dwThreadFocusCookie = TF_INVALID_COOKIE;
        _dwThreadMgrEventCookie = TF_INVALID_COOKIE;
        return;
    }

    ITfSource* source = nullptr;
    const DWORD thread_focus_cookie = _dwThreadFocusCookie;
    const DWORD thread_mgr_cookie = _dwThreadMgrEventCookie;
    const HRESULT source_hr = _threadMgr->QueryInterface(
        IID_ITfSource, reinterpret_cast<void**>(&source));
    HRESULT thread_focus_hr = E_NOINTERFACE;
    HRESULT thread_mgr_hr = E_NOINTERFACE;
    bool thread_focus_attempted = false;
    bool thread_mgr_attempted = false;
    if (SUCCEEDED(source_hr) && source) {
        if (thread_focus_cookie != TF_INVALID_COOKIE) {
            thread_focus_attempted = true;
            thread_focus_hr = source->UnadviseSink(thread_focus_cookie);
        }
        if (thread_mgr_cookie != TF_INVALID_COOKIE) {
            thread_mgr_attempted = true;
            thread_mgr_hr = source->UnadviseSink(thread_mgr_cookie);
        }
        source->Release();
    }
    cxxime_tsf::trace_thread_sinks(
        "unadvise", source_hr,
        thread_focus_attempted, thread_focus_hr, thread_focus_cookie,
        thread_mgr_attempted, thread_mgr_hr, thread_mgr_cookie);
    _dwThreadFocusCookie = TF_INVALID_COOKIE;
    _dwThreadMgrEventCookie = TF_INVALID_COOKIE;
}

HRESULT TextService::_register_key_event_sink() {
    if (!_threadMgr)
        return E_FAIL;

    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (FAILED(_threadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr)))
        return E_FAIL;

    HRESULT hr = pKeystrokeMgr->AdviseKeyEventSink(_clientId, static_cast<ITfKeyEventSink*>(this), TRUE);
    pKeystrokeMgr->Release();
    return hr;
}

HRESULT TextService::_unregister_key_event_sink() {
    if (!_threadMgr)
        return E_FAIL;

    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (FAILED(_threadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr)))
        return E_FAIL;

    HRESULT hr = pKeystrokeMgr->UnadviseKeyEventSink(_clientId);
    pKeystrokeMgr->Release();
    return hr;
}

bool TextService::_register_display_attribute_atom() {
    ITfCategoryMgr* category_mgr = nullptr;
    HRESULT hr = E_UNEXPECTED;
    if ((_activateFlags & TF_TMAE_COMLESS) != 0) {
        hr = cxxime::create_tsf_category_manager_without_com(&category_mgr);
    } else {
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITfCategoryMgr, reinterpret_cast<void**>(&category_mgr));
    }
    if (FAILED(hr) || !category_mgr)
        return false;

    hr = category_mgr->RegisterGUID(c_guidDisplayAttribute, &_displayAttributeAtom);
    if (FAILED(hr)) {
        _displayAttributeAtom = 0;
        category_mgr->Release();
        CXXIME_LOG(L"Register display attribute atom failed: hr=0x%08x", hr);
        return false;
    }
    hr = category_mgr->RegisterGUID(
        c_guidConvertedDisplayAttribute, &_convertedDisplayAttributeAtom);
    if (FAILED(hr)) {
        _convertedDisplayAttributeAtom = 0;
        category_mgr->Release();
        CXXIME_LOG(L"Register converted display attribute atom failed: hr=0x%08x", hr);
        return false;
    }
    hr = category_mgr->RegisterGUID(c_guidFocusedDisplayAttribute, &_focusedDisplayAttributeAtom);
    if (FAILED(hr)) {
        _focusedDisplayAttributeAtom = 0;
        category_mgr->Release();
        CXXIME_LOG(L"Register focused display attribute atom failed: hr=0x%08x", hr);
        return false;
    }
    hr = category_mgr->RegisterGUID(c_guidFocusedConvertedDisplayAttribute,
                                    &_focusedConvertedDisplayAttributeAtom);
    category_mgr->Release();
    if (FAILED(hr)) {
        _focusedConvertedDisplayAttributeAtom = 0;
        CXXIME_LOG(L"Register focused converted display attribute atom failed: hr=0x%08x", hr);
        return false;
    }

    return true;
}

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum)
        return E_INVALIDARG;
    auto* pEnum = new (std::nothrow) ::EnumDisplayAttributeInfo();
    *ppEnum = pEnum;
    return pEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID rguid,
                                                  ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo)
        return E_INVALIDARG;
    *ppInfo = nullptr;

    if (IsEqualGUID(rguid, c_guidDisplayAttribute)) {
        auto* pInfo = new (std::nothrow) ::DisplayAttributeInfo(rguid, TF_ATTR_INPUT);
        *ppInfo = pInfo;
        return pInfo ? S_OK : E_OUTOFMEMORY;
    }
    if (IsEqualGUID(rguid, c_guidConvertedDisplayAttribute)) {
        auto* pInfo = new (std::nothrow) ::DisplayAttributeInfo(rguid, TF_ATTR_CONVERTED);
        *ppInfo = pInfo;
        return pInfo ? S_OK : E_OUTOFMEMORY;
    }
    if (IsEqualGUID(rguid, c_guidFocusedDisplayAttribute)) {
        auto* pInfo = new (std::nothrow) ::DisplayAttributeInfo(
            rguid, TF_ATTR_TARGET_NOTCONVERTED);
        *ppInfo = pInfo;
        return pInfo ? S_OK : E_OUTOFMEMORY;
    }
    if (IsEqualGUID(rguid, c_guidFocusedConvertedDisplayAttribute)) {
        auto* pInfo = new (std::nothrow) ::DisplayAttributeInfo(
            rguid, TF_ATTR_TARGET_CONVERTED);
        *ppInfo = pInfo;
        return pInfo ? S_OK : E_OUTOFMEMORY;
    }
    return E_INVALIDARG;
}
