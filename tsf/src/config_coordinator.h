// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_CONFIG_COORDINATOR_H_
#define CXXIME_TSF_CONFIG_COORDINATOR_H_

#include <cstdint>
#include <memory>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/control_protocol.h>

namespace cxxime_tsf {

constexpr UINT WM_CXXIME_CONFIG_CHANGED = WM_APP + 0x314;
constexpr UINT WM_CXXIME_UI_COMMAND = WM_APP + 0x315;
constexpr UINT WM_CXXIME_REFRESH_CAPS_LOCK = WM_APP + 0x316;
constexpr UINT_PTR TIMER_CXXIME_INPUT_INDICATOR_REFRESH = 0xC317;
// A conversion mode change from outside is judged a moment later (TextService::
// _settle_conversion_change): restored by a focus change, or set by a program.
constexpr UINT_PTR TIMER_CXXIME_CONVERSION_SETTLE = 0xC318;

struct ConfigSnapshot {
    cxxime::ConfigGeneration generation;
    std::shared_ptr<const cxxime::Config> config;
};

std::uint32_t allocate_config_subscription_id();
ConfigSnapshot subscribe_config_updates(HWND window, std::uint32_t subscription_id);
void unsubscribe_config_updates(HWND window, std::uint32_t subscription_id);
void shutdown_tsf_log_writer_if_no_config_subscribers();
ConfigSnapshot current_config_snapshot();

} // namespace cxxime_tsf

#endif // CXXIME_TSF_CONFIG_COORDINATOR_H_
