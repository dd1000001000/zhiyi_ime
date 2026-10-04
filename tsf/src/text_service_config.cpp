// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdint>

#include <cxxime/candidate_window.h>

#include "config_coordinator.h"
#include "globals.h"

namespace {

constexpr wchar_t kConfigWindowClass[] = L"ZhiyiIME.Config.Dispatch";

bool register_config_window_class(WNDPROC window_proc) {
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = g_hInst;
    window_class.lpszClassName = kConfigWindowClass;
    if (RegisterClassExW(&window_class)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

bool TextService::_start_config_updates() {
    if (_configWindow) {
        return true;
    }

    if (!register_config_window_class(_config_window_proc)) {
        _config = cxxime::Config();
        return false;
    }

    _configWindow = CreateWindowExW(0, kConfigWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, g_hInst, this);
    if (!_configWindow) {
        _config = cxxime::Config();
        return false;
    }

    _configSubscriptionId = cxxime_tsf::allocate_config_subscription_id();
    cxxime_tsf::ConfigSnapshot snapshot =
        cxxime_tsf::subscribe_config_updates(_configWindow, _configSubscriptionId);
    if (snapshot.config) {
        _configGeneration = snapshot.generation;
        _config = *snapshot.config;
    }
    return true;
}

void TextService::_stop_config_updates() {
    if (!_configWindow) {
        _capsLockRefreshPending = false;
        cxxime_tsf::shutdown_tsf_log_writer_if_no_config_subscribers();
        return;
    }

    cxxime_tsf::unsubscribe_config_updates(_configWindow, _configSubscriptionId);
    DestroyWindow(_configWindow);
    _configWindow = nullptr;
    _capsLockRefreshPending = false;
    _configSubscriptionId = 0;
    _configGeneration = {};
}

void TextService::_apply_config_snapshot() {
    cxxime_tsf::ConfigSnapshot snapshot = cxxime_tsf::current_config_snapshot();
    if (!snapshot.config || snapshot.generation == _configGeneration) {
        return;
    }

    _configGeneration = snapshot.generation;
    _config = *snapshot.config;

    if (!_activated) {
        return;
    }

    _inputIndicator.set_status_visible(_config.status_window.enable);
    _register_switch_keys();  // the switch keys may have changed
    _reportedInputTarget.clear();  // input collection may have been turned on
    _report_input_target();
    if (_localCandidateWindow) {
        _localCandidateWindow->set_config(_config);
    }

    _publish_ui_presentation();
    _update_state_poll_timer();
}

LRESULT CALLBACK TextService::_config_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TextService* service = reinterpret_cast<TextService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        service = static_cast<TextService*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
    }

    if (msg == cxxime_tsf::WM_CXXIME_CONFIG_CHANGED && service &&
        wp == static_cast<WPARAM>(service->_configSubscriptionId)) {
        service->_apply_config_snapshot();
        return 0;
    }
    if (msg == cxxime_tsf::WM_CXXIME_UI_COMMAND && service) {
        service->_drain_ui_commands();
        return 0;
    }
    if (msg == cxxime_tsf::WM_CXXIME_REFRESH_CAPS_LOCK && service) {
        service->_capsLockRefreshPending = false;
        if (service->_activated && service->_inputFocused) {
            service->_refresh_caps_lock_on_focus("focus_deferred");
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == cxxime_tsf::TIMER_CXXIME_INPUT_INDICATOR_REFRESH && service) {
        KillTimer(hwnd, cxxime_tsf::TIMER_CXXIME_INPUT_INDICATOR_REFRESH);
        service->_inputIndicatorRefreshRetryActive = false;
        service->_refresh_input_indicator();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
