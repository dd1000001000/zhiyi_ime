// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <chrono>
#include <cstring>

#include <cxxime/diagnostics_config.h>
#include <cxxime/logging.h>

#include "globals.h"
#include "tsf_trace.h"

namespace {

bool is_shift_key(WPARAM key) {
    return key == VK_LSHIFT || key == VK_RSHIFT || key == VK_SHIFT;
}

bool is_status_key(WPARAM key) {
    return is_shift_key(key) ||
           key == VK_LCONTROL || key == VK_RCONTROL || key == VK_CONTROL ||
           key == VK_LMENU || key == VK_RMENU ||
           key == VK_LWIN || key == VK_RWIN ||
           key == VK_CAPITAL;
}

bool can_start_text_input(WPARAM key, uint32_t modifiers) {
    constexpr uint32_t kControlOrAlt = 0x02 | 0x04;
    if ((modifiers & kControlOrAlt) != 0) {
        return false;
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return true;
    }
    switch (key) {
    case VK_SPACE:
    case VK_OEM_1:
    case VK_OEM_PLUS:
    case VK_OEM_COMMA:
    case VK_OEM_MINUS:
    case VK_OEM_PERIOD:
    case VK_OEM_2:
    case VK_OEM_3:
    case VK_OEM_4:
    case VK_OEM_5:
    case VK_OEM_6:
    case VK_OEM_7:
    case VK_OEM_8:
    case VK_OEM_102:
        return true;
    default:
        return false;
    }
}

bool indicates_unavailable_input_target(const char* block_reason) {
    return block_reason && (std::strcmp(block_reason, "no_context") == 0 ||
                            std::strcmp(block_reason, "context_not_foreground") == 0);
}

}  // namespace

// ITfKeyEventSink
STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
    if (!_activated) {
        return S_OK;
    }

    if (fForeground) {
        if (_synchronize_effective_edit_target_from_thread_mgr("key_sink_focus")) {
            _refresh_caps_lock_on_focus("key_sink_focus");
            _schedule_caps_lock_refresh();
            if (_sessionId && _client.ensure_connected())
                _client.focus_in(_sessionId);
        } else {
            if (_sessionId && _client.is_connected())
                _client.focus_out(_sessionId);
        }
    } else {
        // Switching away from CxxIME: hide status window immediately.
        // OnKillThreadFocus may not fire when switching IMEs within the same thread.
        _clear_effective_edit_target("ime_focus_lost");
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    _fTestKeyUpPending = false;
    if (_fTestKeyDownPending) {
        *pfEaten = TRUE;
        return S_OK;
    }

    bool status_key = is_status_key(wParam);
    const char* test_block_reason = _input_context_block_reason(pic);
    _trace_input_decision(test_block_reason);
    if (test_block_reason && !status_key) {
        cxxime_tsf::trace_context(
            trace_input_id(), trace_composition_id(), pic, _threadMgr,
            "blocked_input_context");
        _clear_effective_edit_target(
            "test_key_context_rejected",
            indicates_unavailable_input_target(test_block_reason));
        if (wParam < _passThroughKeyUps.size()) {
            _passThroughKeyUps.set(wParam);
        }
        *pfEaten = FALSE;
        return S_OK;
    }

    *pfEaten = _ProcessKeyEvent(pic, wParam, lParam, pfEaten);
    if (wParam == VK_CAPITAL) {
        *pfEaten = TRUE;
    }
    _fTestKeyDownPending = *pfEaten != FALSE;

    OutputDebugStringA("[ZhiyiIME] OnTestKeyDown\n");
    CXXIME_LOG(L"OnTestKeyDown: vk=%u, eaten=%d, sessionId=%u", (unsigned int)wParam, *pfEaten, _sessionId);
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    _fTestKeyDownPending = false;
    if (_fTestKeyUpPending) {
        *pfEaten = TRUE;
        return S_OK;
    }
    if (wParam < _passThroughKeyUps.size() && _passThroughKeyUps.test(wParam)) {
        *pfEaten = FALSE;
        return S_OK;
    }

    *pfEaten = (wParam == VK_CAPITAL) ? TRUE : FALSE;
    CXXIME_LOG(L"OnTestKeyUp: vk=%u, sessionId=%u", (unsigned int)wParam, _sessionId);
    if (wParam != VK_CAPITAL && _ProcessKeyUp(wParam, lParam)) {
        *pfEaten = TRUE;
    }
    _fTestKeyUpPending = *pfEaten != FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (_fTestKeyDownPending) {
        _fTestKeyDownPending = false;
        *pfEaten = TRUE;
        return S_OK;
    }
    // Some apps call OnKeyDown without OnTestKeyDown (e.g. QQ2012)
    *pfEaten = _ProcessKeyEvent(pic, wParam, lParam, pfEaten);
        if (wParam == VK_CAPITAL) {
        *pfEaten = TRUE;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (_fTestKeyUpPending) {
        _fTestKeyUpPending = false;
        *pfEaten = TRUE;
        return S_OK;
    }
    if (wParam < _passThroughKeyUps.size() && _passThroughKeyUps.test(wParam)) {
        _passThroughKeyUps.reset(wParam);
        *pfEaten = FALSE;
        return S_OK;
    }
    if (wParam == VK_CAPITAL) {
        *pfEaten = TRUE;
        return S_OK;
    }
    // Some apps call OnKeyUp without OnTestKeyUp
    *pfEaten = _ProcessKeyUp(wParam, lParam) ? TRUE : FALSE;
    return S_OK;
}

bool TextService::_ProcessKeyEvent(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    *pfEaten = FALSE;
    _hostTraceSession.begin_input(static_cast<uint32_t>(wParam), lParam);
    if (wParam < _passThroughKeyUps.size()) {
        _passThroughKeyUps.reset(wParam);
    }

    bool status_key = is_status_key(wParam);
    uint32_t modifiers = _get_modifiers();
    const char* block_reason = _input_context_block_reason(pic);
    const bool starts_new_composition =
        !_composing && !status_key && _chinese_mode && can_start_text_input(wParam, modifiers);
    const bool should_validate_edit_target =
        !_inputFocused || !_effectiveEditTarget.valid() || starts_new_composition ||
        !_context_matches_effective_edit_target(pic);
    bool input_allowed = block_reason == nullptr;
    _trace_input_decision(block_reason);
    if (!input_allowed && !status_key) {
        cxxime_tsf::trace_context(
            trace_input_id(), trace_composition_id(), pic, _threadMgr,
            "blocked_input_context");
        cxxime_tsf::trace_key_route(
            trace_input_id(), trace_composition_id(), static_cast<uint32_t>(wParam), 0, 0,
            "blocked", block_reason ? block_reason : "input_context");
        _clear_effective_edit_target(
            "key_context_rejected",
            indicates_unavailable_input_target(block_reason));
        if (wParam < _passThroughKeyUps.size()) {
            _passThroughKeyUps.set(wParam);
        }
        return false;
    }

    if (input_allowed && should_validate_edit_target) {
        input_allowed = _synchronize_effective_edit_target(
            pic, nullptr, "key_event");
        if (!input_allowed && !status_key) {
            if (wParam < _passThroughKeyUps.size()) {
                _passThroughKeyUps.set(wParam);
            }
            return false;
        }
    }

    const bool input_was_focused = _inputFocused;
    _inputFocused = input_allowed;
    if (_inputFocused) {
        _update_state_poll_timer();
        if (!input_was_focused) {
            _show_status_window_if_allowed("show:key_edit_target");
        }
    } else {
        _update_state_poll_timer();
        _hide_status_window("hide:key_context_status_only");
        _hide_candidate_window("hide:key_context_status_only");
        _end_reading_ui_element("hide:key_context_status_only_reading");
        _AbortComposition();
    }
    if (wParam == VK_CAPITAL) {
        // Windows reports VK_CAPITAL after the lock bit has toggled. CxxIME's
        // engine expects the final CapsLock state, so do not infer it from the
        // cached IME state, which may lag when focus moved through non-input UI.
        bool target_caps_lock = _is_caps_lock_on();
        if (target_caps_lock)
            modifiers |= 0x08;
        else
            modifiers &= ~0x08;
    }
    bool physical_caps_lock = (modifiers & 0x08) != 0;
    if (wParam != VK_CAPITAL && physical_caps_lock != _caps_lock)
        _sync_caps_lock_state(physical_caps_lock, "key_event");

    // Record key event start time
    _key_event_start = std::chrono::steady_clock::now();

    CXXIME_LOG(L"_ProcessKeyEvent: vk=%u, mods=%u, composing=%d", (unsigned int)wParam, modifiers, _composing);

    cxxime::IPCResponse response = {};
    uint32_t engine_calls = 0;
    auto process_key = [&]() {
        ++engine_calls;
        return _client.process_key(
            _sessionId, static_cast<uint32_t>(wParam), modifiers, response, false,
            _candidate_ui_context());
    };
    auto ipc_start = std::chrono::steady_clock::now();
    bool ok = _ensure_ipc_session() && process_key();
    if (ok && response.status == cxxime::IPCStatus::ERR_INVALID_SESSION) {
        ok = false;
        if (_recreate_ipc_session_preserving_status()) {
            response = {};
            ok = process_key();
        }
    }
    auto ipc_end = std::chrono::steady_clock::now();
    _last_ipc_us = std::chrono::duration_cast<std::chrono::microseconds>(ipc_end - ipc_start).count();

    // If IPC failed, reconnect and create a fresh server session.
    if (!ok) {
        _client.disconnect();
        _ipcHealthy = false;
        if (_recreate_ipc_session_preserving_status()) {
            CXXIME_LOG(L"Reconnected, new sessionId=%u", _sessionId);
            ipc_start = std::chrono::steady_clock::now();
            response = {};
            ok = process_key();
            ipc_end = std::chrono::steady_clock::now();
            _last_ipc_us = std::chrono::duration_cast<std::chrono::microseconds>(ipc_end - ipc_start).count();
        }
    }
    _ipcHealthy = ok;
    cxxime_tsf::trace_key_route(
        trace_input_id(), trace_composition_id(), static_cast<uint32_t>(wParam), modifiers,
        engine_calls, ok ? "processed" : "ipc_failed");

    // Build trace (populated at all exit paths)
    TsfTrace trace;
    trace.vk = (uint32_t)wParam;
    trace.modifiers = modifiers;
    trace.ipc_us = _last_ipc_us;

    if (!ok) {
        CXXIME_LOG(L"_ProcessKeyEvent: IPC FAILED for vk=%u, sessionId=%u", (unsigned int)wParam, _sessionId);
        trace.result = TsfResult::IPC_FAILED;
        auto total_end = std::chrono::steady_clock::now();
        trace.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - _key_event_start).count();
        {
            cxxime::DiagnosticsConfig diag = cxxime::diagnostics_config();
            trace.slow = (trace.ipc_us >= diag.slow_ipc_us) ||
                         (trace.total_us >= diag.slow_total_us);
        }
        _enqueue_trace(trace);
        return false;
    }

    CXXIME_LOG(L"_ProcessKeyEvent: ok, vk=%u, ascii=%u, commit_len=%u, "
               L"preedit_len=%u, composing=%u",
               (unsigned int)wParam, response.ascii_mode,
               static_cast<unsigned int>(strnlen_s(response.commit_text,
                                                   sizeof(response.commit_text))),
               static_cast<unsigned int>(strnlen_s(response.preedit, sizeof(response.preedit))),
               response.composing);

    const bool response_applied = _apply_engine_response(pic, response, pfEaten, &trace);
    if (!response_applied && !*pfEaten && wParam < _passThroughKeyUps.size()) {
        _passThroughKeyUps.set(wParam);
    }

    // Finalize and enqueue trace (async, non-blocking)
    auto total_end = std::chrono::steady_clock::now();
    trace.total_us = std::chrono::duration_cast<std::chrono::microseconds>(total_end - _key_event_start).count();
    {
        cxxime::DiagnosticsConfig diag = cxxime::diagnostics_config();
        trace.slow = (trace.ipc_us >= diag.slow_ipc_us) ||
                     (trace.window_us >= diag.slow_window_us) ||
                     (trace.total_us >= diag.slow_total_us);
    }
    _enqueue_trace(trace);

    cxxime_tsf::trace_key_result(
        trace_input_id(), trace_composition_id(), static_cast<uint32_t>(wParam), *pfEaten != FALSE,
        strnlen_s(response.preedit, sizeof(response.preedit)), response.preedit_cursor,
        response.candidate_count,
        strnlen_s(response.commit_text, sizeof(response.commit_text)), trace.result_string());

    if (trace.result == TsfResult::COMMITTED || trace.result == TsfResult::CLEARED) {
        _reset_trace_composition(trace.result == TsfResult::COMMITTED ? "commit" : "clear");
    }

    return *pfEaten != FALSE;
}

bool TextService::_ProcessKeyUp(WPARAM wParam, LPARAM lParam) {
    if (wParam == VK_CAPITAL) {
        return false;
    }

    _hostTraceSession.begin_input(static_cast<uint32_t>(wParam), lParam);

    uint32_t modifiers = _get_modifiers();
    CXXIME_LOG(L"_ProcessKeyUp: vk=%u, mods=%u, sessionId=%u", (unsigned int)wParam, modifiers,
               _sessionId);

    cxxime::IPCResponse response = {};
    uint32_t engine_calls = 0;
    auto process_key_up = [&]() {
        ++engine_calls;
        return _client.process_key(
            _sessionId, static_cast<uint32_t>(wParam), modifiers, response, true);
    };
    bool ok = _ensure_ipc_session() && process_key_up();
    if (ok && response.status == cxxime::IPCStatus::ERR_INVALID_SESSION) {
        ok = false;
        if (_recreate_ipc_session_preserving_status()) {
            response = {};
            ok = process_key_up();
        }
    }

    // If IPC failed, reconnect and create a fresh server session.
    if (!ok) {
        CXXIME_LOG(L"_ProcessKeyUp: IPC failed, attempting reconnect");
        _client.disconnect();
        _sessionId = 0;
        _ipcHealthy = false;
        if (_recreate_ipc_session_preserving_status()) {
            CXXIME_LOG(L"_ProcessKeyUp: Reconnected, new sessionId=%u", _sessionId);
            response = {};
            ok = process_key_up();
        }
    }
    _ipcHealthy = ok;
    cxxime_tsf::trace_key_route(
        trace_input_id(), trace_composition_id(), static_cast<uint32_t>(wParam), modifiers,
        engine_calls, ok ? "processed_key_up" : "ipc_failed_key_up");

    CXXIME_LOG(L"_ProcessKeyUp: ok=%d, ascii_mode=%u, commit_len=%u, composing=%u",
               ok, response.ascii_mode,
               static_cast<unsigned int>(strnlen_s(response.commit_text,
                                         sizeof(response.commit_text))),
               response.composing);

    BOOL eaten = FALSE;
    if (ok) {
        ITfContext* context = _current_edit_context_for_composition();
        _apply_engine_response(context, response, &eaten);
        if (context) {
            context->Release();
        }
    }

    const bool handled = ok && response.status == cxxime::IPCStatus::OK &&
                         (response.key_handled || eaten != FALSE);
    cxxime_tsf::trace_key_result(
        trace_input_id(), trace_composition_id(), static_cast<uint32_t>(wParam), handled,
        strnlen_s(response.preedit, sizeof(response.preedit)), response.preedit_cursor,
        response.candidate_count,
        strnlen_s(response.commit_text, sizeof(response.commit_text)),
        ok ? "key_up" : "key_up_failed");
    return handled;
}

// No preserved keys are registered: the switch shortcuts are configurable and matched by the
// engine.
STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) {
    if (pfEaten) {
        *pfEaten = FALSE;
    }
    return S_OK;
}

uint32_t TextService::_get_modifiers() const {
    BYTE kb[256] = {};
    uint32_t mods = 0;
    if (GetKeyboardState(kb)) {
        if (kb[VK_SHIFT] & 0x80)
            mods |= 0x01;
        if (kb[VK_CONTROL] & 0x80)
            mods |= 0x02;
        if (kb[VK_MENU] & 0x80)
            mods |= 0x04;
        if (kb[VK_CAPITAL] & 0x1)
            mods |= 0x08;
    }
    return mods;
}
