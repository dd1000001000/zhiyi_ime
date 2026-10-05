// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_INPUT_INDICATOR_CONTROLLER_H_
#define CXXIME_TSF_INPUT_INDICATOR_CONTROLLER_H_

#include <cstdint>
#include <functional>

#include <windows.h>
#include <msctf.h>

#include <cxxime/ime_menu.h>
#include <cxxime/ipc_protocol.h>

class CLangBarItemButton;

namespace cxxime_tsf {

class InputIndicatorController {
public:
    using ToggleChineseCallback = std::function<void()>;
    using MenuCommandCallback = std::function<void(cxxime::ImeMenuCommand)>;

    InputIndicatorController();
    ~InputIndicatorController();

    InputIndicatorController(const InputIndicatorController&) = delete;
    InputIndicatorController& operator=(const InputIndicatorController&) = delete;

    bool initialize(ITfThreadMgr* thread_manager, TfClientId client_id,
                    const cxxime::ImeStatus& initial_status,
                    ToggleChineseCallback toggle_chinese_callback,
                    MenuCommandCallback menu_command_callback,
                    bool menu_chinese);
    void shutdown();
    void update_from_status(const cxxime::ImeStatus& status);
    void set_menu_language(bool chinese);
    bool reconcile_host_registration();

private:
    enum class RegistrationState {
        kNotRegistered,
        kRegistered,
        kUnknown,
    };

    ITfThreadMgr* thread_manager_ = nullptr;
    CLangBarItemButton* button_ = nullptr;
    RegistrationState registration_state_ = RegistrationState::kNotRegistered;
};

} // namespace cxxime_tsf

#endif // CXXIME_TSF_INPUT_INDICATOR_CONTROLLER_H_
