// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <cxxime/logging.h>

#include "config_coordinator.h"

namespace {

constexpr UINT kIpcHeartbeatIntervalMs = 1500;
constexpr auto kIpcHeartbeatInterval = std::chrono::milliseconds(kIpcHeartbeatIntervalMs);
constexpr int kTsfIpcTimeoutMs = 800;
constexpr UINT kStatePollFastIntervalMs = 30;
constexpr unsigned int kEditTargetValidationFailureLimit = 2;
constexpr UINT kInputIndicatorRefreshRetryDelaysMs[] = {100, 500, 2000, 5000};

std::mutex g_state_poll_timer_mutex;
std::unordered_map<UINT_PTR, TextService*> g_state_poll_timers;

bool same_visible_status(const cxxime::ImeStatus& a, const cxxime::ImeStatus& b) {
    return a.flags == b.flags && a.input_mode == b.input_mode;
}

}  // namespace

void TextService::_sync_ime_status(const cxxime::ImeStatus& status) {
    bool local_changed = _chinese_mode != status.chinese_mode() ||
                         _caps_lock != status.caps_lock();
    bool visible_changed = true;
    bool status_became_available = false;
    _chinese_mode = status.chinese_mode();
    _caps_lock = status.caps_lock();
    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        status_became_available = !_has_synced_ime_status();
        visible_changed = status_became_available || !same_visible_status(_lastImeStatus, status);
        _lastImeStatus = status;
        _hasLastImeStatus.store(true, std::memory_order_release);
    }
    if (!local_changed && !visible_changed) {
        return;
    }

    // Update button state before notifying the compartment. The language bar
    // queries GetIcon during the notification, so stale button state causes a
    // second refresh and visible flicker.
    _inputIndicator.update_from_status(status);
    if (!_handlingConversionCompartmentChange) {
        _sync_conversion_mode_compartment(status);
    }
    _publish_ui_presentation();
}

bool TextService::_is_caps_lock_on() const {
    BYTE kb[256] = {};
	if (GetKeyboardState(kb)) {
        return (kb[VK_CAPITAL] & 0x01) != 0;
    }

    return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
}

bool TextService::_ensure_ipc_session() {
    if (_sessionId && _client.is_connected())
        return true;

    if (!_client.is_connected() &&
        !_client.connect(cxxime::IPC_PIPE_BASE_NAME, kTsfIpcTimeoutMs)) {
        if (_ipcHealthy) {
            CXXIME_LOG(L"IPC unavailable");
            _enqueue_event_trace("ipc_session", "connect_failed", true);
        }
        _ipcHealthy = false;
        return false;
    }

    uint32_t session_id = 0;
    constexpr uint64_t kClientCapabilities =
        cxxime::kClientCapabilitySegmentedSelection |
        cxxime::kClientCapabilitySegmentedPreeditPresentation;
    if (!_client.start_session(session_id, kClientCapabilities) ||
        session_id == 0) {
        CXXIME_LOG(L"Failed to start IPC session");
        if (_ipcHealthy)
            _enqueue_event_trace("ipc_session", "start_failed", true);
        _sessionId = 0;
        _ipcHealthy = false;
        _client.disconnect();
        return false;
    }

    _sessionId = session_id;
    ++_uiSessionGeneration;
    if (_uiSessionGeneration == 0) {
        ++_uiSessionGeneration;
    }
    _start_ui_presentation_channel();
    _ipcHealthy = true;
    _lastIpcHeartbeat = std::chrono::steady_clock::now();
    CXXIME_LOG(L"IPC session ready, sessionId=%u", _sessionId);
    _enqueue_event_trace("ipc_session", "ready");
    return true;
}

bool TextService::_recreate_ipc_session_preserving_status() {
    cxxime::ImeStatus desired_status = {};
    bool has_desired_status = false;
    {
        std::lock_guard<std::mutex> lock(_lastImeStatusMutex);
        has_desired_status = _has_synced_ime_status();
        if (has_desired_status) {
            desired_status = _lastImeStatus;
        }
    }
    bool desired_chinese_mode = has_desired_status ? desired_status.chinese_mode() : _chinese_mode;
    bool input_allows_input = _query_input_focus_from_thread_mgr();
    bool physical_caps_lock = false;

    _publish_ui_session_ended();
    _sessionId = 0;
    _ipcHealthy = false;
    if (!_ensure_ipc_session())
        return false;

    cxxime::ImeStatus synced_status = {};
    if (input_allows_input) {
        physical_caps_lock = _is_caps_lock_on();
        if (!_sync_caps_lock_state(physical_caps_lock, "session_recreate", &synced_status)) {
            return false;
        }
    } else {
        cxxime::IPCResponse status_resp = {};
        if (!_client.get_status(_sessionId, status_resp) ||
            status_resp.status != cxxime::IPCStatus::OK) {
            return false;
        }
        synced_status = status_resp.ime_status;
        _sync_ime_status(synced_status);
    }

    if (!synced_status.caps_lock() && synced_status.chinese_mode() != desired_chinese_mode) {
        cxxime::IPCResponse mode_resp = {};
        if (_client.set_chinese_mode(_sessionId, desired_chinese_mode, mode_resp) &&
            mode_resp.status == cxxime::IPCStatus::OK) {
            synced_status = mode_resp.ime_status;
            _sync_ime_status(mode_resp.ime_status);
        } else {
            return false;
        }
    }
    if (has_desired_status && synced_status.input_mode != desired_status.input_mode) {
        cxxime::IPCResponse mode_resp = {};
        if (_client.switch_input_mode(_sessionId, desired_status.input_mode, mode_resp) &&
            mode_resp.status == cxxime::IPCStatus::OK) {
            synced_status = mode_resp.ime_status;
            _sync_ime_status(mode_resp.ime_status);
        } else {
            return false;
        }
    }
    if (has_desired_status && synced_status.full_shape() != desired_status.full_shape()) {
        cxxime::IPCResponse shape_resp = {};
        if (_client.toggle_shape(_sessionId, shape_resp) &&
            shape_resp.status == cxxime::IPCStatus::OK) {
            synced_status = shape_resp.ime_status;
            _sync_ime_status(shape_resp.ime_status);
        } else {
            return false;
        }
    }
    if (has_desired_status && synced_status.chinese_punct() != desired_status.chinese_punct()) {
        cxxime::IPCResponse punct_resp = {};
        if (_client.toggle_punct(_sessionId, punct_resp) &&
            punct_resp.status == cxxime::IPCStatus::OK) {
            _sync_ime_status(punct_resp.ime_status);
        } else {
            return false;
        }
    }
    _enqueue_event_trace("ipc_session", "recreated", true);
    return true;
}

bool TextService::_heartbeat_ipc() {
    if (!_activated || !_sessionId)
        return false;

    auto now = std::chrono::steady_clock::now();
    if (_lastIpcHeartbeat.time_since_epoch().count() != 0 &&
        now - _lastIpcHeartbeat < kIpcHeartbeatInterval) {
        return _ipcHealthy;
    }
    _lastIpcHeartbeat = now;

    if (!_client.ensure_connected()) {
        CXXIME_LOG(L"IPC heartbeat reconnect failed");
        if (_ipcHealthy)
            _enqueue_event_trace("ipc_session", "heartbeat_reconnect_failed", true);
        _publish_ui_session_ended();
        _sessionId = 0;
        _ipcHealthy = false;
        return false;
    }

    cxxime::IPCResponse resp = {};
    if (_client.get_status(_sessionId, resp) && resp.status == cxxime::IPCStatus::OK) {
        _ipcHealthy = true;
        _sync_ime_status(resp.ime_status);
        return true;
    }
    if (resp.status == cxxime::IPCStatus::ERR_INVALID_SESSION) {
        CXXIME_LOG(L"IPC heartbeat detected invalid session, recreating");
        _enqueue_event_trace("ipc_session", "heartbeat_invalid_session", true);
        return _recreate_ipc_session_preserving_status();
    }

    CXXIME_LOG(L"IPC heartbeat failed, reconnecting");
    if (_ipcHealthy)
        _enqueue_event_trace("ipc_session", "heartbeat_failed", true);
    _client.disconnect();
    _publish_ui_session_ended();
    _sessionId = 0;
    _ipcHealthy = false;
    return false;
}

bool TextService::_refresh_input_indicator() {
    if (_inputIndicator.reconcile_host_registration()) {
        _stop_input_indicator_refresh_retry();
        _inputIndicatorRefreshRetryCount = 0;
        return true;
    }
    _schedule_input_indicator_refresh_retry();
    return false;
}

void TextService::_schedule_input_indicator_refresh_retry() {
    if (!_configWindow || _inputIndicatorRefreshRetryActive ||
        _inputIndicatorRefreshRetryCount >=
        sizeof(kInputIndicatorRefreshRetryDelaysMs) /
            sizeof(kInputIndicatorRefreshRetryDelaysMs[0])) {
            return;
    }
    const UINT delay =
        kInputIndicatorRefreshRetryDelaysMs[_inputIndicatorRefreshRetryCount++];
    if (SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_INPUT_INDICATOR_REFRESH,
        delay, nullptr)) {
        _inputIndicatorRefreshRetryActive = true;
    }
}

void TextService::_stop_input_indicator_refresh_retry() {
    if (_inputIndicatorRefreshRetryActive && _configWindow) {
        KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_INPUT_INDICATOR_REFRESH);
    }
    _inputIndicatorRefreshRetryActive = false;
}

bool TextService::_sync_caps_lock_state(bool caps_lock,
                                        const char* source,
                                        cxxime::ImeStatus* synced_status) {
    if (!_ensure_ipc_session())
        return false;

    cxxime::IPCResponse resp = {};
    if (_client.sync_caps_lock(_sessionId, caps_lock, resp) &&
        resp.status == cxxime::IPCStatus::OK) {
        if (synced_status)
            *synced_status = resp.ime_status;
        _sync_ime_status(resp.ime_status);
        char detail[96] = {};
        snprintf(detail, sizeof(detail), "source=%s value=%d",
                 source ? source : "unknown", caps_lock ? 1 : 0);
        _enqueue_event_trace("caps_lock_sync", detail, true);
        return true;
    }
    return false;
}

bool TextService::_refresh_caps_lock_on_focus(const char* source) {
    const bool physical_caps_lock = _is_caps_lock_on();
    if (physical_caps_lock == _caps_lock) {
        return true;
    }

    return _sync_caps_lock_state(physical_caps_lock, source);
}

void TextService::_schedule_caps_lock_refresh() {
    if (!_configWindow || _capsLockRefreshPending) {
        return;
    }
    _capsLockRefreshPending =
        PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_REFRESH_CAPS_LOCK, 0, 0) != FALSE;
}

void TextService::_update_state_poll_timer() {
    if (!_activated || _inputTargetUnavailable) {
        _stop_state_poll_timer();
        return;
    }

    const bool track_candidate =
        _inputFocused && _candidatePresentation.external_window_expected() &&
        _candidatePresentation.caret_poll_pending();
    const UINT interval = track_candidate ? kStatePollFastIntervalMs : kIpcHeartbeatIntervalMs;
    if (_statePollTimer && _statePollIntervalMs == interval)
        return;

    _stop_state_poll_timer();

    UINT_PTR timer = SetTimer(nullptr, 0, interval, _state_poll_timer_proc);
    if (!timer)
        return;

    {
        std::lock_guard<std::mutex> lock(g_state_poll_timer_mutex);
        g_state_poll_timers[timer] = this;
    }
    _statePollTimer = timer;
    _statePollIntervalMs = interval;
    char detail[64] = {};
    snprintf(detail, sizeof(detail), "interval_ms=%u", static_cast<unsigned int>(interval));
    _enqueue_event_trace("state_poll_timer", detail, true);
}

void TextService::_stop_state_poll_timer() {
    if (!_statePollTimer)
        return;

    UINT_PTR timer = _statePollTimer;
    _statePollTimer = 0;
    _statePollIntervalMs = 0;
    {
        std::lock_guard<std::mutex> lock(g_state_poll_timer_mutex);
        g_state_poll_timers.erase(timer);
    }
    KillTimer(nullptr, timer);
}

VOID CALLBACK TextService::_state_poll_timer_proc(HWND, UINT, UINT_PTR id_event, DWORD) {
    TextService* service = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_poll_timer_mutex);
        auto it = g_state_poll_timers.find(id_event);
        if (it != g_state_poll_timers.end())
            service = it->second;
    }
    if (service)
        service->_poll_runtime_state();
}

void TextService::_poll_runtime_state() {
    if (!_activated)
        return;

    _heartbeat_ipc();
    if (!_sessionId || !_inputFocused)
        return;

    if (_effectiveContext && _effectiveEditTarget.valid()) {
        if (_context_belongs_to_foreground(_effectiveContext)) {
            _runtimeTargetValidationFailures = 0;
        } else if (++_runtimeTargetValidationFailures >= kEditTargetValidationFailureLimit) {
            _clear_effective_edit_target("runtime_target_validation", true);
            return;
        }
    }

    if (!_candidatePresentation.external_window_expected() ||
        !_candidatePresentation.caret_poll_pending()) {
        _update_state_poll_timer();
        return;
    }

    if (_composing && _candidatePresentation.external_window_expected() &&
         _candidatePresentation.waiting_for_caret()) {
        _follow_native_caret();
    }
    RECT provisional_rect = {};
    if (_candidatePresentation.external_window_expected() &&
        _candidatePresentation.accept_provisional_caret_after_timeout(
            cxxime_tsf::CandidatePresentation::Clock::now(), &provisional_rect)) {
        set_caret_rect(provisional_rect);
        trace_caret_event("initial_timeout", "provisional", true, &provisional_rect, S_FALSE,
                          true);
        _publish_ui_presentation();
    }
    if (_candidatePresentation.external_window_expected() &&
        _candidatePresentation.caret_jump_pending() &&
        !_candidatePresentation.waiting_for_caret() &&
        _candidatePresentation.accept_pending_caret_after_timeout(
            cxxime_tsf::CandidatePresentation::Clock::now())) {
        trace_caret_event("jump_timeout", "stabilizer", true, &_caretRect, S_FALSE, true);
        _publish_ui_presentation();
    }
    if (_candidatePresentation.external_window_expected() &&
        _candidatePresentation.caret_jump_pending() &&
        !_candidatePresentation.waiting_for_caret()) {
        ITfContext* context = _current_edit_context_for_composition();
        if (context) {
            _request_candidate_position_update(context, "caret_jump_confirm");
            context->Release();
        }
    }
    if (_candidatePresentation.external_window_expected() &&
        _candidatePresentation.waiting_for_caret()) {
        const auto now = cxxime_tsf::CandidatePresentation::Clock::now();
        const bool fallback_due = _candidatePresentation.pending_caret_fallback_due(
            now, static_cast<int>(kStatePollFastIntervalMs));
        const bool expired = _candidatePresentation.expire_caret_wait(now);
        ITfContext* context = _current_edit_context_for_composition();
        if (context) {
            if (fallback_due) {
                RECT fallback_rect = _resolve_caret_rect(context);
                const bool resolved = cxxime_tsf::is_valid_caret_rect(fallback_rect);
                trace_caret_event("pending_timeout", "fallback", resolved, &fallback_rect,
                                  resolved ? S_FALSE : E_FAIL, true);
                if (resolved) {
                    update_candidate_position(fallback_rect, context, false,
                                              _candidatePresentation.generation());
                }
            }
            if (expired && _candidatePresentation.waiting_for_caret() &&
                _caretRectTargetGeneration == _uiTargetGeneration &&
                _context_matches_effective_edit_target(context) &&
                cxxime_tsf::is_valid_caret_rect(_caretRect) &&
                _candidatePresentation.accept_caret(_candidatePresentation.generation())) {
                // Finalize an observed extent without manufacturing a new caret sample.
                _publish_ui_presentation();
            }
            if (!expired && _candidatePresentation.waiting_for_caret()) {
                _request_candidate_position_update(context, "show:pending_timeout");
            }
            context->Release();
        }
        if (expired) {
            trace_caret_event("wait_timeout", "event_only", false, nullptr, S_FALSE, true);
        }
    }
    _update_state_poll_timer();
}
