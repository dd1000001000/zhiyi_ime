// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cxxime/logging.h>

#include "engine_response.h"
#include "tsf_activation.h"
#include "tsf_imm_mode.h"

namespace {

class ScopedFlag {
public:
    explicit ScopedFlag(bool& flag) : flag_(flag), previous_(flag) {
        flag_ = true;
    }

    ~ScopedFlag() {
        flag_ = previous_;
    }

private:
    bool& flag_;
    bool previous_;
};

} // namespace

void TextService::_register_conversion_compartment_sink() {
    _unregister_conversion_compartment_sink();

    ITfCompartmentMgr* compartment_manager = nullptr;
    const HRESULT manager_hr = _threadMgr
        ? _threadMgr->QueryInterface(
              IID_ITfCompartmentMgr,
              reinterpret_cast<void**>(&compartment_manager))
        : E_POINTER;

    HRESULT compartment_hr = E_UNEXPECTED;
    if (SUCCEEDED(manager_hr) && compartment_manager) {
        compartment_hr = compartment_manager->GetCompartment(
            GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
            &_conversionCompartment);
        compartment_manager->Release();
    }

    HRESULT source_hr = E_UNEXPECTED;
    if (SUCCEEDED(compartment_hr) && _conversionCompartment) {
        source_hr = _conversionCompartment->QueryInterface(
            IID_ITfSource,
            reinterpret_cast<void**>(&_conversionCompartmentSource));
    }

    HRESULT advise_hr = E_UNEXPECTED;
    if (SUCCEEDED(source_hr) && _conversionCompartmentSource) {
        advise_hr = _conversionCompartmentSource->AdviseSink(
            IID_ITfCompartmentEventSink,
            static_cast<ITfCompartmentEventSink*>(this),
            &_dwConversionCompartmentCookie);
    }

    cxxime_tsf::trace_conversion_sink_lifecycle(
        "advise", manager_hr, compartment_hr, source_hr, advise_hr,
        _dwConversionCompartmentCookie);
    if (FAILED(advise_hr)) {
        CXXIME_LOG(L"Conversion compartment sink registration failed: hr=0x%08x",
                   advise_hr);
        _unregister_conversion_compartment_sink();
    }
}

void TextService::_unregister_conversion_compartment_sink() {
    const DWORD cookie = _dwConversionCompartmentCookie;
    HRESULT unadvise_hr = S_FALSE;
    if (_conversionCompartmentSource && cookie != TF_INVALID_COOKIE) {
        unadvise_hr = _conversionCompartmentSource->UnadviseSink(cookie);
    }
    if (_conversionCompartmentSource ||
        _conversionCompartment ||
        cookie != TF_INVALID_COOKIE) {
        cxxime_tsf::trace_conversion_sink_lifecycle(
            "unadvise", S_OK, S_OK, S_OK, unadvise_hr, cookie);
    }

    _dwConversionCompartmentCookie = TF_INVALID_COOKIE;
    if (_conversionCompartmentSource) {
        _conversionCompartmentSource->Release();
        _conversionCompartmentSource = nullptr;
    }
    if (_conversionCompartment) {
        _conversionCompartment->Release();
        _conversionCompartment = nullptr;
    }
}

void TextService::_register_open_close_compartment_sink() {
    _unregister_open_close_compartment_sink();
    ITfCompartmentMgr* compartment_manager = nullptr;
    if (!_threadMgr || FAILED(_threadMgr->QueryInterface(
                           IID_ITfCompartmentMgr,
                           reinterpret_cast<void**>(&compartment_manager)))) {
        return;
    }
    const HRESULT hr = compartment_manager->GetCompartment(
        GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &_openCloseCompartment);
    compartment_manager->Release();
    if (FAILED(hr) || !_openCloseCompartment ||
        FAILED(_openCloseCompartment->QueryInterface(
            IID_ITfSource, reinterpret_cast<void**>(&_openCloseCompartmentSource))) ||
        FAILED(_openCloseCompartmentSource->AdviseSink(
            IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(this),
            &_dwOpenCloseCompartmentCookie))) {
        cxxime_tsf::trace_activation_step("open_close_sink", "failed", hr, false);
        _unregister_open_close_compartment_sink();
    }
}

void TextService::_unregister_open_close_compartment_sink() {
    if (_openCloseCompartmentSource && _dwOpenCloseCompartmentCookie != TF_INVALID_COOKIE) {
        _openCloseCompartmentSource->UnadviseSink(_dwOpenCloseCompartmentCookie);
    }
    _dwOpenCloseCompartmentCookie = TF_INVALID_COOKIE;
    if (_openCloseCompartmentSource) {
        _openCloseCompartmentSource->Release();
        _openCloseCompartmentSource = nullptr;
    }
    if (_openCloseCompartment) {
        _openCloseCompartment->Release();
        _openCloseCompartment = nullptr;
    }
}

cxxime::KeyboardShortcut TextService::_switch_key(int slot) const {
    switch (slot) {
    case 0: return _config.ascii_toggle_shortcut;
    case 1: return _config.english_style_shortcut;
    case 2: return _config.punct_toggle_shortcut;
    case 3: return _config.shape_toggle_shortcut;
    default: return {};
    }
}

// The key state of the message being handled (GetKeyState), so a press is recognized while
// Windows handles its hotkey.
int TextService::_held_switch_key() const {
    auto down = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
    for (int slot = 0; slot < 4; ++slot) {
        const cxxime::KeyboardShortcut key = _switch_key(slot);
        if (!key.enabled() || !down(static_cast<int>(key.virtual_key))) continue;
        if (down(VK_CONTROL) == ((key.modifiers & cxxime::kKeyModifierControl) != 0) &&
            down(VK_MENU) == ((key.modifiers & cxxime::kKeyModifierAlt) != 0) &&
            down(VK_SHIFT) == ((key.modifiers & cxxime::kKeyModifierShift) != 0)) {
            return slot;
        }
    }
    return -1;
}

bool TextService::_claim_switch_key_press(int slot) {
    constexpr ULONGLONG kSamePressMs = 300;
    if (slot < 0 || slot >= 4) return false;
    const ULONGLONG now = GetTickCount64();
    if (_switchKeyTicks[slot] != 0 && now - _switchKeyTicks[slot] < kSamePressMs) return false;
    _switchKeyTicks[slot] = now;
    return true;
}

// A system input method hotkey changed a compartment: when a configured switch key is held,
// that key is run (once per press) and the compartment change itself is not applied.
bool TextService::_run_held_switch_key() {
    const int slot = _held_switch_key();
    if (slot < 0 || slot == 1) return false;  // the style key is no system hotkey
    if (_claim_switch_key_press(slot) && !_apply_switch_key(slot)) {
        _switchKeyTicks[slot] = 0;  // not done: the key event may still do it
    }
    return true;
}

bool TextService::_apply_switch_key(int slot) {
    if (slot != 0 && slot != 2 && slot != 3) return false;
    cxxime::IPCResponse response = {};
    if (!_ensure_ipc_session()) return false;
    const bool sent = slot == 0 ? _client.set_chinese_mode(_sessionId, !_chinese_mode, response)
                      : slot == 2 ? _client.toggle_punct(_sessionId, response)
                                  : _client.toggle_shape(_sessionId, response);
    if (!sent || response.status != cxxime::IPCStatus::OK) return false;
    if (slot == 0) {
        // Like a status change from outside: an open composition is committed as typed.
        ITfContext* context = _current_edit_context_for_composition();
        BOOL eaten = FALSE;
        _apply_engine_response(context, response, &eaten);
        if (context) context->Release();
    } else {
        _sync_ime_status(response.ime_status);
    }
    cxxime_tsf::trace_activation_step("switch_key", "applied", S_OK, false);
    return true;
}

HRESULT TextService::_read_conversion_mode_compartment(
    DWORD* conversion_mode,
    VARTYPE* value_type) const {
    if (!conversion_mode || !value_type) {
        return E_INVALIDARG;
    }
    *conversion_mode = 0;
    *value_type = VT_EMPTY;
    if (!_conversionCompartment) {
        return E_POINTER;
    }

    VARIANT value = {};
    VariantInit(&value);
    const HRESULT value_hr = _conversionCompartment->GetValue(&value);
    *value_type = value.vt;
    HRESULT result = value_hr;
    if (SUCCEEDED(value_hr)) {
        if (value.vt == VT_I4 || value.vt == VT_INT) {
            *conversion_mode = static_cast<DWORD>(value.lVal);
        } else if (value.vt == VT_UI4 || value.vt == VT_UINT) {
            *conversion_mode = value.ulVal;
        } else {
            result = DISP_E_TYPEMISMATCH;
        }
    }
    VariantClear(&value);
    return result;
}

STDMETHODIMP TextService::OnChange(REFGUID rguid) {
    if (IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
        // Only a system hotkey on a configured switch key matters (e.g. Ctrl+Space).
        if (_activated) _run_held_switch_key();
        return S_OK;
    }
    if (!IsEqualGUID(
            rguid, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
        return S_OK;
    }
    if (_activated && !_writingConversionCompartment && _run_held_switch_key()) {
        return S_OK;  // e.g. Shift+Space or Ctrl+. changed the conversion mode
    }

    DWORD conversion_mode = 0;
    VARTYPE value_type = VT_EMPTY;
    const HRESULT value_hr = _read_conversion_mode_compartment(
        &conversion_mode, &value_type);
    const bool before_chinese = _chinese_mode;
    const bool requested_chinese =
        (conversion_mode & TF_CONVERSIONMODE_NATIVE) != 0;
    const bool self_write = _writingConversionCompartment;
    const bool was_composing = _composing;
    ScopedFlag handling_change(_handlingConversionCompartmentChange);

    cxxime_tsf::TraceConversionSinkChange trace_event;
    trace_event.value_hr = value_hr;
    trace_event.value_type = static_cast<uint16_t>(value_type);
    trace_event.conversion_mode = conversion_mode;
    trace_event.self_write = self_write;
    trace_event.composing = was_composing;
    trace_event.before_chinese = before_chinese;
    trace_event.requested_chinese = requested_chinese;

    auto trace_change = [&](bool set_attempted,
                             bool set_succeeded,
                             bool commit_requested,
                             uint32_t commit_text_length,
                             const char* result) {
        trace_event.set_attempted = set_attempted;
        trace_event.set_succeeded = set_succeeded;
        trace_event.commit_requested = commit_requested;
        trace_event.commit_text_length = commit_text_length;
        trace_event.after_chinese = _chinese_mode;
        trace_event.result = result;
        cxxime_tsf::trace_conversion_sink_change(trace_event);
    };

    if (FAILED(value_hr)) {
        trace_change(false, false, false, 0, "read_failed");
        return S_OK;
    }
    if (self_write) {
        trace_change(false, false, false, 0, "self_write");
        return S_OK;
    }
    if (!_activated) {
        trace_change(false, false, false, 0, "inactive");
        return S_OK;
    }
    if (before_chinese == requested_chinese) {
        trace_change(false, false, false, 0, "already_aligned");
        return S_OK;
    }

    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        if (_has_synced_ime_status()) {
            trace_event.status_details = true;
            trace_event.before_full_shape = _lastImeStatus.full_shape();
            trace_event.before_chinese_punct = _lastImeStatus.chinese_punct();
            trace_event.before_input_mode =
                static_cast<uint32_t>(_lastImeStatus.input_mode);
        }
    }

    cxxime::IPCResponse response = {};
    const bool set_succeeded =
        _ensure_ipc_session() &&
        _client.set_chinese_mode(_sessionId, requested_chinese, response) &&
        response.status == cxxime::IPCStatus::OK;
    trace_event.ipc_us = _client.last_ipc_us();
    if (!set_succeeded) {
        trace_change(true, false, false, 0, "set_failed");
        return S_OK;
    }

    trace_event.after_full_shape = response.ime_status.full_shape();
    trace_event.after_chinese_punct = response.ime_status.chinese_punct();
    trace_event.after_input_mode =
        static_cast<uint32_t>(response.ime_status.input_mode);

    std::wstring commit_text;
    const bool commit_requested = response.commit_text[0] != '\0' &&
        cxxime_tsf::decode_engine_commit_text(
            response, &commit_text) &&
        !commit_text.empty();
    const uint32_t commit_text_length = static_cast<uint32_t>(commit_text.length());
    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    const bool applied = _apply_engine_response(context, response, &eaten);
    if (context) {
        context->Release();
    }
    if (!applied) {
        trace_change(true, false, commit_requested, commit_text_length, "response_failed");
        return S_OK;
    }
    if ((was_composing || commit_requested) && !response.composing) {
        _reset_trace_composition("conversion_change_commit");
    }

    trace_change(true, true, commit_requested, commit_text_length,
                 _chinese_mode == requested_chinese ? "applied" : "caps_lock");
    return S_OK;
}
