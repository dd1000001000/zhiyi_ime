// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"
#include "text_before_caret.h"

#include <algorithm>
#include <iterator>
#include <new>

#include <cxxime/logging.h>
#include <cxxime/settings_launcher.h>
#include <cxxime/tsf_factory.h>

#include "about_dialog.h"
#include "config_coordinator.h"
#include "display_attribute.h"
#include "globals.h"
#include "host_compatibility/host_classification_compatibility.h"
#include "tsf_activation.h"
#include "tsf_imm_mode.h"
#include "tsf_ui_element_observer.h"

// The taskbar indicator's menu (cxxime::kImeMenuItems).
void TextService::_handle_ime_menu_command(cxxime::ImeMenuCommand command) {
    cxxime::ImeStatus status;
    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        status = _lastImeStatus;
    }
    cxxime::IPCResponse response = {};
    auto apply_status = [&](bool sent) {
        if (sent && response.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(response.ime_status);
        }
    };
    auto switch_mode = [&](cxxime::InputMode mode) {
        if (status.input_mode == mode) return true;
        response = {};
        const bool sent = _ensure_ipc_session() &&
                          _client.switch_input_mode(_sessionId, mode, response);
        apply_status(sent);
        return sent && response.status == cxxime::IPCStatus::OK;
    };
    auto set_style = [&](bool english_style, bool value) {
        response = {};
        apply_status(_ensure_ipc_session() &&
                     _client.set_input_style(_sessionId, english_style, value, response));
    };

    switch (command) {
    case cxxime::ImeMenuCommand::kChinese:
    case cxxime::ImeMenuCommand::kEnglish: {
        const bool chinese = command == cxxime::ImeMenuCommand::kChinese;
        if (chinese == _chinese_mode) break;
        // Like a switch key: an open composition is committed as typed.
        if (_ensure_ipc_session() && _client.set_chinese_mode(_sessionId, chinese, response) &&
            response.status == cxxime::IPCStatus::OK) {
            ITfContext* context = _current_edit_context_for_composition();
            BOOL eaten = FALSE;
            _apply_engine_response(context, response, &eaten);
            if (context) context->Release();
        }
        break;
    }
    case cxxime::ImeMenuCommand::kPinyin:
    case cxxime::ImeMenuCommand::kPinyinInitials:
        if (switch_mode(cxxime::InputMode::PINYIN)) {
            set_style(false, command == cxxime::ImeMenuCommand::kPinyinInitials);
        }
        break;
    case cxxime::ImeMenuCommand::kWubi:
        switch_mode(cxxime::InputMode::WUBI);
        break;
    case cxxime::ImeMenuCommand::kMixed:
        switch_mode(cxxime::InputMode::MIXED);
        break;
    case cxxime::ImeMenuCommand::kEnglishWords:
        set_style(true, !status.english_words());
        break;
    case cxxime::ImeMenuCommand::kChinesePunct:
        apply_status(_ensure_ipc_session() && _client.toggle_punct(_sessionId, response));
        break;
    case cxxime::ImeMenuCommand::kFullShape:
        apply_status(_ensure_ipc_session() && _client.toggle_shape(_sessionId, response));
        break;
    case cxxime::ImeMenuCommand::kDictionary:
    case cxxime::ImeMenuCommand::kSettings: {
        const cxxime::SettingsPanel panel = command == cxxime::ImeMenuCommand::kDictionary
                                                ? cxxime::SettingsPanel::kDictionary
                                                : cxxime::SettingsPanel::kInput;
        // The server opens settings (outside this program's sandbox); without a server (after
        // Exit, or startup.autostart off) this program does, and settings starts the server.
        // The menu click made this app the foreground one; the server and the settings program
        // are background processes, so pass the right to bring a window forward on to them
        // (otherwise the settings window opens behind this app).
        AllowSetForegroundWindow(ASFW_ANY);
        if ((!_ensure_ipc_session() || !_client.open_settings(_sessionId, panel)) &&
            !cxxime::open_settings(panel)) {
            CXXIME_LOG(L"%s", L"settings_request source=tsf result=0");
        }
        break;
    }
    case cxxime::ImeMenuCommand::kAbout:
        show_about_dialog();
        break;
    case cxxime::ImeMenuCommand::kExit:
        if (_ensure_ipc_session() && _client.exit_server(_sessionId)) {
            _client.disconnect();
            _sessionId = 0;
        } else {
            CXXIME_LOG(L"%s", L"exit_request source=tsf result=0");
        }
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

    _register_switch_keys();

    cxxime_tsf::trace_activation_step("conversion_sink", "attempt", S_OK, false);
    _register_conversion_compartment_sink();
    _register_open_close_compartment_sink();
    cxxime_tsf::trace_activation_step("conversion_sink", "complete", S_OK, false);
}

void TextService::_synchronize_activation_focus() {
    if (_synchronize_effective_edit_target_from_thread_mgr("activate_complete")) {
        _refresh_caps_lock_on_focus("activate_complete");
        _schedule_caps_lock_refresh();
        if (_sessionId && _client.ensure_connected()) {
            _client.focus_in(_sessionId);
            _report_input_target();
        }
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

namespace {

std::string utf8_of(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                            size, nullptr, nullptr);
    }
    return out;
}

}  // namespace

void TextService::_report_input_target() {
    if (!_config.experience_program || !_config.collect_input || !_sessionId) {
        _reportedInputTarget.clear();
        return;
    }
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring app = module;
    const size_t slash = app.find_last_of(L"\\/");
    if (slash != std::wstring::npos) app.erase(0, slash + 1);
    wchar_t title[256] = {};
    if (HWND window = GetForegroundWindow()) {
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    }
    const std::wstring target = app + L"\n" + title;
    if (target == _reportedInputTarget) return;
    if (_client.set_input_target(_sessionId, utf8_of(app), utf8_of(title))) {
        _reportedInputTarget = target;
    }
}

void TextService::_send_text_before_caret(ITfContext* context) {
    constexpr size_t kMaxChars = 256;  // more than the model gets (laya.*context_chars)
    if (!_sessionId || !context) return;
    std::wstring text;
    const TextBeforeCaret read = read_text_before_caret(context, _clientId, kMaxChars, &text);
    uint32_t flags = 0;
    // The context alone does not tell input boxes apart where the IMM layer serves several
    // edit windows of a thread with one context: the focused window does.
    const uintptr_t target = reinterpret_cast<uintptr_t>(context) ^
                             (reinterpret_cast<uintptr_t>(GetFocus()) << 1);
    if (target != _contextReadTarget) {
        flags |= cxxime::kContextNewInputBox;
        _contextReadTarget = target;
    }
    if (read == TextBeforeCaret::kPrivate) text.clear();  // a password: no context at all
    if (read != TextBeforeCaret::kUnavailable) flags |= cxxime::kContextTextRead;
    char line[128];
    snprintf(line, sizeof(line), "[ZhiyiIME] context: %s, %u chars%s\n",
             read == TextBeforeCaret::kRead ? "read" : read == TextBeforeCaret::kPrivate ? "private" : "unavailable",
             static_cast<unsigned>(text.size()), (flags & cxxime::kContextNewInputBox) ? ", new input box" : "");
    OutputDebugStringA(line);  // lengths only, never the text
    if (flags == 0) return;  // the same box, unreadable: keep what the server remembers
    _client.set_context(_sessionId, utf8_of(text), flags);
}

void TextService::_register_switch_keys() {
    _unregister_switch_keys();
    if (!_threadMgr) return;
    ITfKeystrokeMgr* keystroke_mgr = nullptr;
    if (FAILED(_threadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                          reinterpret_cast<void**>(&keystroke_mgr)))) {
        return;
    }
    const struct {
        const GUID* guid;
        cxxime::KeyboardShortcut key;
        const wchar_t* description;
    } keys[] = {
        {&c_guidPreservedKeyAsciiToggle, _config.ascii_toggle_shortcut, L"Chinese/English"},
        {&c_guidPreservedKeyStyle, _config.english_style_shortcut, L"Input style"},
        {&c_guidPreservedKeyPunct, _config.punct_toggle_shortcut, L"Punctuation"},
        {&c_guidPreservedKeyShape, _config.shape_toggle_shortcut, L"Full/half width"},
    };
    for (size_t i = 0; i < std::size(keys); ++i) {
        if (!keys[i].key.enabled()) continue;
        TF_PRESERVEDKEY preserved = {};
        preserved.uVKey = keys[i].key.virtual_key;
        if (keys[i].key.modifiers & cxxime::kKeyModifierControl) preserved.uModifiers |= TF_MOD_CONTROL;
        if (keys[i].key.modifiers & cxxime::kKeyModifierAlt) preserved.uModifiers |= TF_MOD_ALT;
        if (keys[i].key.modifiers & cxxime::kKeyModifierShift) preserved.uModifiers |= TF_MOD_SHIFT;
        const HRESULT hr = keystroke_mgr->PreserveKey(
            _clientId, *keys[i].guid, &preserved, keys[i].description,
            static_cast<ULONG>(wcslen(keys[i].description)));
        if (SUCCEEDED(hr)) {
            _preservedSwitchKeys[i] = keys[i].key;
        } else {
            // E.g. another text service preserved the same key in this thread.
            cxxime_tsf::trace_activation_step("switch_key", "preserve_failed", hr, false);
        }
    }
    keystroke_mgr->Release();
}

void TextService::_unregister_switch_keys() {
    ITfKeystrokeMgr* keystroke_mgr = nullptr;
    if (_threadMgr && SUCCEEDED(_threadMgr->QueryInterface(
                          IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystroke_mgr)))) {
        const GUID* guids[] = {&c_guidPreservedKeyAsciiToggle, &c_guidPreservedKeyStyle,
                               &c_guidPreservedKeyPunct, &c_guidPreservedKeyShape};
        for (size_t i = 0; i < std::size(guids); ++i) {
            if (!_preservedSwitchKeys[i].enabled()) continue;
            TF_PRESERVEDKEY preserved = {};
            preserved.uVKey = _preservedSwitchKeys[i].virtual_key;
            const uint32_t m = _preservedSwitchKeys[i].modifiers;
            if (m & cxxime::kKeyModifierControl) preserved.uModifiers |= TF_MOD_CONTROL;
            if (m & cxxime::kKeyModifierAlt) preserved.uModifiers |= TF_MOD_ALT;
            if (m & cxxime::kKeyModifierShift) preserved.uModifiers |= TF_MOD_SHIFT;
            keystroke_mgr->UnpreserveKey(*guids[i], &preserved);
        }
        keystroke_mgr->Release();
    }
    for (auto& key : _preservedSwitchKeys) key = {};
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
