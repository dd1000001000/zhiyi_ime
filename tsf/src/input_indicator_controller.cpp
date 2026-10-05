// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "input_indicator_controller.h"

#include <utility>

#include <cxxime/logging.h>

#include "language_bar.h"

namespace cxxime_tsf {

InputIndicatorController::InputIndicatorController() = default;

InputIndicatorController::~InputIndicatorController() { shutdown(); }

bool InputIndicatorController::initialize(ITfThreadMgr* thread_manager, TfClientId client_id,
                                          const cxxime::ImeStatus& initial_status,
                                          ToggleChineseCallback toggle_chinese_callback,
                                          MenuCommandCallback menu_command_callback,
                                          bool menu_chinese) {
    shutdown();
    if (!thread_manager || client_id == TF_CLIENTID_NULL) {
        return false;
    }

    thread_manager_ = thread_manager;
    thread_manager_->AddRef();
    button_ = new CLangBarItemButton(client_id, GUID_LBI_INPUTMODE);
    button_->update_from_status(initial_status);
    button_->set_toggle_chinese_callback(std::move(toggle_chinese_callback));
    button_->set_menu_command_callback(std::move(menu_command_callback));
    button_->set_menu_language(menu_chinese);

    ITfLangBarItemMgr* manager = nullptr;
    const HRESULT query_hr =
        thread_manager_->QueryInterface(IID_ITfLangBarItemMgr, (void**)&manager);
    if (FAILED(query_hr)) {
        CXXIME_LOG(L"input_indicator event=query_manager result=0 hr=0x%08x", query_hr);
        return false;
    }
    const HRESULT add_hr = manager->AddItem(button_);
    manager->Release();
    registration_state_ =
        SUCCEEDED(add_hr) ? RegistrationState::kRegistered : RegistrationState::kNotRegistered;
    CXXIME_LOG(L"input_indicator event=register result=%d hr=0x%08x",
               registration_state_ == RegistrationState::kRegistered ? 1 : 0, add_hr);
    return registration_state_ == RegistrationState::kRegistered;
}

void InputIndicatorController::shutdown() {
    if (button_) {
        // The language bar manager may retain the item when RemoveItem fails.
        // Detach callbacks before releasing TextService-owned state.
        button_->set_toggle_chinese_callback({});
        button_->set_menu_command_callback({});
    }
    if (thread_manager_ && button_) {
        ITfLangBarItemMgr* manager = nullptr;
        if (SUCCEEDED(thread_manager_->QueryInterface(IID_ITfLangBarItemMgr, (void**)&manager))) {
            const HRESULT remove_hr = manager->RemoveItem(button_);
            if (FAILED(remove_hr)) {
                CXXIME_LOG(L"input_indicator event=shutdown_remove result=0 hr=0x%08x",
                            remove_hr);
            }
            manager->Release();
        }
    }
    registration_state_ = RegistrationState::kNotRegistered;
    if (button_) {
        button_->Release();
        button_ = nullptr;
    }
    if (thread_manager_) {
        thread_manager_->Release();
        thread_manager_ = nullptr;
    }
}

void InputIndicatorController::update_from_status(const cxxime::ImeStatus& status) {
    if (button_) {
        button_->update_from_status(status);
    }
}

void InputIndicatorController::set_menu_language(bool chinese) {
    if (button_) {
        button_->set_menu_language(chinese);
    }
}

bool InputIndicatorController::reconcile_host_registration() {
    if (!thread_manager_ || !button_) {
        return false;
    }

    ITfLangBarItemMgr* manager = nullptr;
    const HRESULT query_hr =
        thread_manager_->QueryInterface(IID_ITfLangBarItemMgr, (void**)&manager);
    if (FAILED(query_hr)) {
        CXXIME_LOG(L"input_indicator event=reconcile_query result=0 hr=0x%08x", query_hr);
        return false;
    }

    bool remove_succeeded = true;
    if (registration_state_ != RegistrationState::kNotRegistered) {
        const HRESULT remove_hr = manager->RemoveItem(button_);
        if (FAILED(remove_hr)) {
            registration_state_ = RegistrationState::kUnknown;
            remove_succeeded = false;
            CXXIME_LOG(L"input_indicator event=reconcile_remove result=0 hr=0x%08x", remove_hr);
        } else {
            registration_state_ = RegistrationState::kNotRegistered;
        }
    }
    const HRESULT add_hr = manager->AddItem(button_);
    manager->Release();
    if (FAILED(add_hr)) {
        registration_state_ =
            remove_succeeded ? RegistrationState::kNotRegistered : RegistrationState::kUnknown;
        button_->notify_full_update();
        CXXIME_LOG(L"input_indicator event=reconcile result=0 hr=0x%08x", add_hr);
        return false;
    }

    registration_state_ = RegistrationState::kRegistered;
    button_->notify_full_update();
    CXXIME_LOG(L"%s", L"input_indicator event=reconcile result=1");
    return true;
}

} // namespace cxxime_tsf
