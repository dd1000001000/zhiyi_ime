// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <windows.h>
#include <wtsapi32.h>

#include "system_lifecycle_monitor.h"
#include "support/testutil.h"

#ifndef WTS_SESSION_DESKTOP_READY
#define WTS_SESSION_DESKTOP_READY 0xF
#endif

namespace {

HWND create_test_window() {
    return CreateWindowExW(0, L"STATIC", L"Zhiyi IME lifecycle test", WS_OVERLAPPED,
                           CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

} // namespace

TEST(SystemLifecycleMonitor, ignores_unpaired_events_and_coalesces_recovery) {
    HWND window = create_test_window();
    ASSERT_TRUE(window != nullptr);

    int reconcile_count = 0;
    SystemLifecycleMonitor::Event last_event =
        SystemLifecycleMonitor::Event::kTaskbarCreated;
    SystemLifecycleMonitor lifecycle;
    lifecycle.start(window, [&](SystemLifecycleMonitor::Event event) {
        last_event = event;
        ++reconcile_count;
    });

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_CONNECT, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_DESKTOP_READY, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    ASSERT_EQ(reconcile_count, 0);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ASSERT_EQ(reconcile_count, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    ASSERT_EQ(reconcile_count, 1);
    ASSERT_EQ(last_event, SystemLifecycleMonitor::Event::kSessionResumed);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    ASSERT_EQ(reconcile_count, 2);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ASSERT_EQ(reconcile_count, 3);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
    ASSERT_EQ(reconcile_count, 3);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_CONNECT, 0);
    ASSERT_EQ(reconcile_count, 3);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_DESKTOP_READY, 0);
    ASSERT_EQ(reconcile_count, 3);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    ASSERT_EQ(reconcile_count, 3);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_DISCONNECT, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_CONNECT, 0);
    ASSERT_EQ(reconcile_count, 4);

    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_REMOTE_DISCONNECT, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_DESKTOP_READY, 0);
    ASSERT_EQ(reconcile_count, 5);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMECRITICAL, 0);
    ASSERT_EQ(reconcile_count, 6);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMECRITICAL, 0);
    ASSERT_EQ(reconcile_count, 7);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    ASSERT_EQ(reconcile_count, 7);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ASSERT_EQ(reconcile_count, 8);

    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_DISCONNECT, 0);
    lifecycle.handle_message(WM_WTSSESSION_CHANGE, WTS_CONSOLE_CONNECT, 0);
    ASSERT_EQ(reconcile_count, 8);
    lifecycle.handle_message(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ASSERT_EQ(reconcile_count, 9);

    lifecycle.stop();
    DestroyWindow(window);
}

RUN_ALL_TESTS()
