// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "probe_app.h"

#include "ui_element_sink.h"

#include <cxxime/host_trace.h>
#include <cxxime/tsf_factory.h>

#include <ctfutb.h>

#include <iomanip>
#include <new>
#include <sstream>

namespace cxxime_probe {

namespace {

const char* com_mode_name(ProbeComMode mode) {
    switch (mode) {
    case ProbeComMode::sta:
        return "sta";
    case ProbeComMode::uninitialized:
        return "uninitialized";
    case ProbeComMode::mta:
        return "mta";
    }
    return "unknown";
}

} // namespace

const std::wstring& ProbeApp::initialization_error() const {
    return initialization_error_;
}

bool ProbeApp::fail_initialization(const char* step, HRESULT result) {
    std::wostringstream message;
    message << L"Failed to initialize the IME host Probe.\n\nStep: " << step
            << L"\nHRESULT: 0x" << std::uppercase << std::hex << std::setw(8)
            << std::setfill(L'0') << static_cast<uint32_t>(result);
    initialization_error_ = message.str();

    cxxime::write_host_trace("probe", "probe.initialization", {
        {"step", step},
        {"hresult", static_cast<uint32_t>(result)},
        {"com_mode", com_mode_name(com_mode_)},
        {"activate_flags", activate_flags_},
        {"result", "failed"},
    });
    return false;
}

bool ProbeApp::initialize(HINSTANCE instance, ProbeComMode com_mode) {
    instance_ = instance;
    com_mode_ = com_mode;
    activate_flags_ = TF_TMAE_UIELEMENTENABLEDONLY;
    if (com_mode_ != ProbeComMode::sta) {
        activate_flags_ |= TF_TMAE_COMLESS;
    }
    if (com_mode_ != ProbeComMode::uninitialized) {
        DWORD apartment = COINIT_APARTMENTTHREADED;
        if (com_mode_ == ProbeComMode::mta) {
            apartment = COINIT_MULTITHREADED;
        }
        const HRESULT com_result = CoInitializeEx(nullptr, apartment);
        com_initialized_ = SUCCEEDED(com_result);
        if (FAILED(com_result) &&
            !(com_mode_ == ProbeComMode::sta && com_result == RPC_E_CHANGED_MODE)) {
            return fail_initialization("CoInitializeEx", com_result);
        }
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance_;
    window_class.lpfnWndProc = window_proc;
    window_class.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return fail_initialization("RegisterClassExW", HRESULT_FROM_WIN32(error));
        }
    }

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Zhiyi IME Host Candidate Probe",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 560,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        const DWORD error = GetLastError();
        return fail_initialization("CreateWindowExW(main)", HRESULT_FROM_WIN32(error));
    }
    if (!RegisterHotKey(hwnd_, kConversionHotKeyId, MOD_NOREPEAT, VK_F8)) {
        const DWORD error = GetLastError();
        return fail_initialization("RegisterHotKey(F8)", HRESULT_FROM_WIN32(error));
    }

    gate_checkbox_ = CreateWindowExW(
        0, L"BUTTON", L"Require composition/reading signal to show candidates",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 20, 470, 26, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGateCheckboxId)), instance_, nullptr);
    original_ui_checkbox_ = CreateWindowExW(
        0, L"BUTTON", L"Automatically test original TIP UI handoff",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 50, 390, 26, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOriginalUiCheckboxId)),
        instance_, nullptr);
    himc_ = ImmGetContext(hwnd_);
    if (!initialize_tsf_runtime()) {
        return false;
    }
    initialize_conversion_compartment_probe();

    const HRESULT manager_result = thread_mgr_->QueryInterface(
        IID_ITfUIElementMgr, reinterpret_cast<void**>(&ui_element_mgr_));
    if (FAILED(manager_result) || !ui_element_mgr_) {
        return fail_initialization("QueryInterface(ITfUIElementMgr)", manager_result);
    }

    // Microsoft requires the UI element sink source to be obtained from ITfUIElementMgr.
    const HRESULT source_result = ui_element_mgr_->QueryInterface(
        IID_ITfSource, reinterpret_cast<void**>(&source_));
    if (FAILED(source_result) || !source_) {
        return fail_initialization("ITfUIElementMgr::QueryInterface(ITfSource)", source_result);
    }

    sink_ = new (std::nothrow) UiElementSink(this);
    if (!sink_) {
        return fail_initialization("UiElementSink allocation", E_OUTOFMEMORY);
    }
    const HRESULT advise_result = source_->AdviseSink(
        IID_ITfUIElementSink, static_cast<ITfUIElementSink*>(sink_), &sink_cookie_);
    if (FAILED(advise_result)) {
        return fail_initialization("ITfSource::AdviseSink(ITfUIElementSink)", advise_result);
    }

    cxxime::write_host_trace("probe", "probe.runtime", {
        {"hwnd", reinterpret_cast<uintptr_t>(hwnd_)},
        {"himc", reinterpret_cast<uintptr_t>(himc_)},
        {"activate_flags", activate_flags_},
        {"com_mode", com_mode_name(com_mode_)},
        {"thread_manager_factory",
         com_mode_ == ProbeComMode::sta ? "CoCreateInstance" : "TF_CreateThreadMgr"},
        {"client_id", client_id_},
        {"result", "ready"},
    });
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetFocus(hwnd_);
    return true;
}

bool ProbeApp::initialize_tsf_runtime() {
    HRESULT create_result = E_UNEXPECTED;
    if (com_mode_ == ProbeComMode::sta) {
        create_result =
            CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgrEx,
                             reinterpret_cast<void**>(&thread_mgr_));
    } else {
        ITfThreadMgr* thread_manager = nullptr;
        create_result = cxxime::create_tsf_thread_manager_without_com(&thread_manager);
        if (SUCCEEDED(create_result) && thread_manager) {
            create_result = thread_manager->QueryInterface(IID_ITfThreadMgrEx,
                                                           reinterpret_cast<void**>(&thread_mgr_));
            thread_manager->Release();
        }
    }
    if (FAILED(create_result) || !thread_mgr_) {
        return fail_initialization("CreateThreadMgr", create_result);
    }

    const HRESULT activate_result = thread_mgr_->ActivateEx(&client_id_, activate_flags_);
    if (FAILED(activate_result)) {
        return fail_initialization("ITfThreadMgrEx::ActivateEx", activate_result);
    }
    thread_mgr_active_ = true;
    return true;
}

int ProbeApp::run() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void ProbeApp::shutdown() {
    if (hwnd_) {
        KillTimer(hwnd_, kCandidateUiVisibilityTimerId);
        UnregisterHotKey(hwnd_, kConversionHotKeyId);
    }
    if (source_ && sink_cookie_ != TF_INVALID_COOKIE) {
        source_->UnadviseSink(sink_cookie_);
        sink_cookie_ = TF_INVALID_COOKIE;
    }
    if (sink_) {
        sink_->Release();
        sink_ = nullptr;
    }
    if (source_) {
        source_->Release();
        source_ = nullptr;
    }
    if (ui_element_mgr_) {
        ui_element_mgr_->Release();
        ui_element_mgr_ = nullptr;
    }
    shutdown_conversion_compartment_probe();
    if (thread_mgr_) {
        if (thread_mgr_active_) {
            thread_mgr_->Deactivate();
        }
        thread_mgr_->Release();
        thread_mgr_ = nullptr;
    }
    if (himc_ && hwnd_) {
        ImmReleaseContext(hwnd_, himc_);
        himc_ = nullptr;
    }
    if (com_initialized_) {
        CoUninitialize();
    }
}

} // namespace cxxime_probe
