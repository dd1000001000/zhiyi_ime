// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <limits>
#include <vector>

#include <windows.h>
#include <commctrl.h>

#include <cxxime/status_window.h>
#include <cxxime/window_position.h>

#include "support/dpi_testutil.h"
#include "support/testutil.h"

static bool create_test_window(cxxime::StatusWindow& window) {
    const cxxime::StatusTheme theme;
    return window.create(theme);
}

static int scale_status_metric(HWND hwnd, int metric) {
    return static_cast<int>(metric * (GetDpiForWindow(hwnd) / 96.0f) + 0.5f);
}

static POINT status_button_center(HWND hwnd, int index) {
    int x = scale_status_metric(hwnd, 6);
    x += scale_status_metric(hwnd, 28);
    x += scale_status_metric(hwnd, 4);
    for (int i = 0; i < index; ++i) {
        x += scale_status_metric(hwnd, i < 3 ? 28 : 24);
        x += scale_status_metric(hwnd, 4);
        if (i == 2) {
            x += scale_status_metric(hwnd, 2 * 8 + 1);
        }
    }

    const int width = scale_status_metric(hwnd, index < 3 ? 28 : 24);
    const int center_y = scale_status_metric(hwnd, 6) + scale_status_metric(hwnd, 22) / 2;
    return {x + width / 2, center_y};
}

static POINT input_mode_center(HWND hwnd) {
    return {
        scale_status_metric(hwnd, 6) + scale_status_metric(hwnd, 28) / 2,
        scale_status_metric(hwnd, 6) + scale_status_metric(hwnd, 22) / 2,
    };
}

static void send_mouse_at_screen_point(HWND hwnd, UINT message, POINT point) {
    ScreenToClient(hwnd, &point);
    SendMessageW(hwnd, message, 0, MAKELPARAM(point.x, point.y));
}

static std::vector<HMONITOR> monitor_handles() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

struct DpiInjection {
    UINT values[3];
    int submissions = 0;
    int depth = 0;
    int max_depth = 0;
};

static LRESULT CALLBACK inject_dpi_on_move(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                           DWORD_PTR data) {
    auto& injection = *reinterpret_cast<DpiInjection*>(data);
    if (message != WM_WINDOWPOSCHANGED) {
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    ++injection.depth;
    if (injection.depth > injection.max_depth) {
        injection.max_depth = injection.depth;
    }
    const int index = injection.submissions++;
    if (index < 3) {
        RECT suggested = {};
        GetWindowRect(hwnd, &suggested);
        const UINT dpi = injection.values[index];
        SendMessageW(hwnd, WM_DPICHANGED, MAKELPARAM(dpi, dpi),
                     reinterpret_cast<LPARAM>(&suggested));
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    --injection.depth;
    return result;
}

// ============================================================
// Create / Destroy
// ============================================================

TEST(StatusWindow, LifecycleOperationsAreIdempotent) {
    cxxime::StatusWindow window;
    window.destroy();
    ASSERT_TRUE(!window.is_created());

    ASSERT_TRUE(create_test_window(window));
    ASSERT_TRUE(window.is_created());
    ASSERT_TRUE(GetWindow(window.hwnd_for_test(), GW_OWNER) == nullptr);
    const HWND first_window = window.hwnd_for_test();

    ASSERT_TRUE(create_test_window(window));
    ASSERT_EQ(window.hwnd_for_test(), first_window);

    window.destroy();
    window.destroy();
    ASSERT_TRUE(!window.is_created());

    ASSERT_TRUE(create_test_window(window));
    ASSERT_TRUE(window.is_created());
    window.destroy();
    ASSERT_TRUE(!window.is_created());
}

// ============================================================
// Show / Hide
// ============================================================

TEST(StatusWindow, ShowHidePreservesTopmostStyle) {
    cxxime::StatusWindow window;
    window.show();
    window.hide();
    ASSERT_TRUE(!window.is_created());

    ASSERT_TRUE(create_test_window(window));
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    ASSERT_TRUE(SetWindowPos(window.hwnd_for_test(), HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE);
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);

    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    window.hide();
    ASSERT_TRUE(!window.is_visible());
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_TRUE((GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    window.destroy();
}

// ============================================================
// Position
// ============================================================

TEST(StatusWindow, ClampPositionToWorkArea) {
    const RECT work_area = {-1600, 120, 1600, 1080};

    POINT position = cxxime::clamp_window_position_to_work_area(-1000, 300, 200, 100, work_area);
    ASSERT_EQ(position.x, -1000);
    ASSERT_EQ(position.y, 300);

    position = cxxime::clamp_window_position_to_work_area(-2000, 0, 200, 100, work_area);
    ASSERT_EQ(position.x, -1600);
    ASSERT_EQ(position.y, 120);

    position = cxxime::clamp_window_position_to_work_area(1500, 1000, 200, 100, work_area);
    ASSERT_EQ(position.x, 1400);
    ASSERT_EQ(position.y, 980);

    position = cxxime::clamp_window_position_to_work_area(300, 500, 4000, 100, work_area);
    ASSERT_EQ(position.x, -1600);
    ASSERT_EQ(position.y, 500);

    position = cxxime::clamp_window_position_to_work_area(-800, 500, 200, 2000, work_area);
    ASSERT_EQ(position.x, -800);
    ASSERT_EQ(position.y, 120);

    position = cxxime::clamp_window_position_to_work_area(-500, 200, 4000, 1000, work_area);
    ASSERT_EQ(position.x, -1600);
    ASSERT_EQ(position.y, 120);

    position = cxxime::clamp_window_position_to_work_area(
        std::numeric_limits<int>::max(), std::numeric_limits<int>::min(), 200, 100, work_area);
    ASSERT_EQ(position.x, 1400);
    ASSERT_EQ(position.y, 120);

    position = cxxime::clamp_window_position_to_work_area(1601, 1081, -1, -1, work_area);
    ASSERT_EQ(position.x, 1600);
    ASSERT_EQ(position.y, 1080);
}

TEST(StatusWindow, FullscreenRequiresCoveringTheEntireMonitor) {
    const RECT monitor = {0, 0, 1920, 1080};

    ASSERT_TRUE(cxxime::rect_covers_monitor({0, 0, 1920, 1080}, monitor));
    ASSERT_TRUE(cxxime::rect_covers_monitor({-8, -8, 1928, 1088}, monitor));
    ASSERT_TRUE(!cxxime::rect_covers_monitor({0, 0, 1920, 1040}, monitor));
    ASSERT_TRUE(!cxxime::rect_covers_monitor({0, 40, 1920, 1080}, monitor));
    ASSERT_TRUE(!cxxime::rect_covers_monitor({}, monitor));
}

TEST(StatusWindow, RestoredPositionFitsTheWorkArea) {
    test::ScopedDpiAwarenessContext dpi_context;

    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));

    HWND hwnd = window.hwnd_for_test();
    RECT window_rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &window_rect));
    const POINT virtual_right_edge = {
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1,
        GetSystemMetrics(SM_YVIRTUALSCREEN),
    };
    HMONITOR monitor = MonitorFromPoint(virtual_right_edge, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    ASSERT_TRUE(GetMonitorInfoW(monitor, &monitor_info));

    const int width = window_rect.right - window_rect.left;
    const int partial_x = monitor_info.rcWork.right - width / 2;
    ASSERT_TRUE(SetWindowPos(hwnd, nullptr, partial_x, monitor_info.rcWork.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
    ASSERT_TRUE(GetWindowRect(hwnd, &window_rect));
    ASSERT_EQ(window_rect.left, partial_x);

    int callback_count = 0;
    POINT saved_position = {};
    window.set_position_callback([&](int x, int y) {
        ++callback_count;
        saved_position = {x, y};
    });
    window.set_position(partial_x, monitor_info.rcWork.top);

    ASSERT_TRUE(GetWindowRect(hwnd, &window_rect));
    ASSERT_EQ(window_rect.right, monitor_info.rcWork.right);
    ASSERT_EQ(callback_count, 1);
    ASSERT_EQ(saved_position.x, window_rect.left);
    ASSERT_EQ(saved_position.y, window_rect.top);

    window.destroy();
}

TEST(StatusWindow, DragConstrainsPositionWithoutClickingAndSavesOnRelease) {
    test::ScopedDpiAwarenessContext dpi_context;
    const bool single_monitor = GetSystemMetrics(SM_CMONITORS) == 1;
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));
    const HWND hwnd = window.hwnd_for_test();
    MONITORINFO info = {sizeof(info)};
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info));
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &rect));
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const RECT work = info.rcWork;
    const POINT start = {work.left + (work.right - work.left - width) / 2,
                         work.top + (work.bottom - work.top - height) / 2};
    struct DragCase {
        POINT requested;
        POINT expected;
        POINT slide;
        bool should_test;
    };
    const DragCase cases[] = {
        // Always exercise ordinary dragging, including multi-screen auto-hide desktops.
        {{start.x + width / 2, start.y}, {start.x + width / 2, start.y}, {0, height}, true},
        {{work.left - width / 2, start.y},
         {work.left, start.y},
         {0, height},
         single_monitor || work.left > info.rcMonitor.left},
        {{work.right - width / 2, start.y},
         {work.right - width, start.y},
         {0, height},
         single_monitor || work.right < info.rcMonitor.right},
        {{start.x, work.top - height / 2},
         {start.x, work.top},
         {width, 0},
         single_monitor || work.top > info.rcMonitor.top},
        {{start.x, work.bottom - height / 2},
         {start.x, work.bottom - height},
         {width, 0},
         single_monitor || work.bottom < info.rcMonitor.bottom},
    };
    int click_count = 0;
    window.set_click_callback([&](cxxime::StatusButton) { ++click_count; });
    int saved_count = 0;
    POINT saved = {};
    window.set_position_callback([&](int x, int y) {
        ++saved_count;
        saved = {x, y};
    });
    const POINT anchor = status_button_center(hwnd, 0);
    for (const DragCase& item : cases) {
        if (!item.should_test) {
            continue;
        }
        window.set_position(start.x, start.y);
        saved_count = 0;
        send_mouse_at_screen_point(hwnd, WM_LBUTTONDOWN, {start.x + anchor.x, start.y + anchor.y});
        send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE,
                                   {item.requested.x + anchor.x, item.requested.y + anchor.y});
        ASSERT_TRUE(GetWindowRect(hwnd, &rect));
        ASSERT_EQ(rect.left, item.expected.x);
        ASSERT_EQ(rect.top, item.expected.y);

        const POINT slid_mouse = {item.requested.x + anchor.x + item.slide.x,
                                  item.requested.y + anchor.y + item.slide.y};
        send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE, slid_mouse);
        ASSERT_TRUE(GetWindowRect(hwnd, &rect));
        ASSERT_EQ(rect.left, item.expected.x + item.slide.x);
        ASSERT_EQ(rect.top, item.expected.y + item.slide.y);
        ASSERT_TRUE(rect.left >= work.left && rect.right <= work.right);
        ASSERT_TRUE(rect.top >= work.top && rect.bottom <= work.bottom);
        ASSERT_EQ(saved_count, 0);
        send_mouse_at_screen_point(hwnd, WM_LBUTTONUP, slid_mouse);
        ASSERT_TRUE(GetWindowRect(hwnd, &rect));
        ASSERT_EQ(click_count, 0);
        ASSERT_EQ(saved_count, 1);
        ASSERT_EQ(saved.x, rect.left);
        ASSERT_EQ(saved.y, rect.top);
    }
}

TEST(StatusWindow, LayoutChangesAvoidReservedArea) {
    test::ScopedDpiAwarenessContext dpi_context;
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));
    const HWND hwnd = window.hwnd_for_test();
    MONITORINFO info = {sizeof(info)};
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info));
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &rect));
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const POINT start = {info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
                         info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2};
    POINT requested = start;
    POINT expected = start;
    RECT reserved = info.rcMonitor;
    if (info.rcWork.bottom < info.rcMonitor.bottom) {
        requested.y = info.rcWork.bottom - height / 2;
        expected.y = info.rcWork.bottom - height;
        reserved.top = info.rcWork.bottom;
    } else if (info.rcWork.top > info.rcMonitor.top) {
        requested.y = info.rcWork.top - height / 2;
        expected.y = info.rcWork.top;
        reserved.bottom = info.rcWork.top;
    } else if (info.rcWork.left > info.rcMonitor.left) {
        requested.x = info.rcWork.left - width / 2;
        expected.x = info.rcWork.left;
        reserved.right = info.rcWork.left;
    } else if (info.rcWork.right < info.rcMonitor.right) {
        requested.x = info.rcWork.right - width / 2;
        expected.x = info.rcWork.right - width;
        reserved.left = info.rcWork.right;
    } else {
        // Headless/auto-hide desktops may have no reserved work-area edge.
        return;
    }
    // Derive the expected contact edge from the OS work area, not the positioning helper.
    RECT overlap = {};

    // Re-evaluate an existing position after work-area or monitor changes.
    const UINT messages[] = {WM_SETTINGCHANGE, WM_DISPLAYCHANGE, WM_DPICHANGED};
    for (UINT message : messages) {
        ASSERT_TRUE(SetWindowPos(hwnd, nullptr, requested.x, requested.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        ASSERT_TRUE(GetWindowRect(hwnd, &rect));
        const WPARAM param =
            message == WM_DPICHANGED ? MAKELPARAM(window.dpi(), window.dpi()) : SPI_SETWORKAREA;
        SendMessageW(hwnd, message, param, reinterpret_cast<LPARAM>(&rect));
        ASSERT_TRUE(GetWindowRect(hwnd, &rect));
        ASSERT_EQ(rect.left, expected.x);
        ASSERT_EQ(rect.top, expected.y);
        ASSERT_TRUE(!IntersectRect(&overlap, &rect, &reserved));
    }
}

TEST(StatusWindow, NestedDpiChangesDoNotRecursivelyMoveOrPublishIntermediatePositions) {
    test::ScopedDpiAwarenessContext dpi_context;
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));
    window.show();
    const HWND hwnd = window.hwnd_for_test();
    MONITORINFO info = {sizeof(info)};
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info));
    // At higher DPI, creation may already clamp the window to the bottom-right corner.
    // Start elsewhere so the first submission produces a real position change.
    window.set_position(info.rcWork.left, info.rcWork.top);
    RECT initial = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &initial));
    // Keep the requested rect within this monitor when choosing the placement target.
    const POINT requested = {info.rcWork.right - (initial.right - initial.left),
    info.rcWork.bottom - (initial.bottom - initial.top)};
    ASSERT_TRUE(initial.left != requested.x || initial.top != requested.y);
    const UINT dpi = window.dpi();
    DpiInjection injection = {{dpi + 24, dpi, dpi + 48}};
    ASSERT_TRUE(
        SetWindowSubclass(hwnd, inject_dpi_on_move, 1, reinterpret_cast<DWORD_PTR>(&injection)));
    int geometry_count = 0;
    int position_count = 0;
    POINT saved = {};
    window.set_geometry_changed_callback([&]() { ++geometry_count; });
    window.set_position_callback([&](int x, int y) {
        ++position_count;
        saved = {x, y};
    });
    window.set_position(requested.x, requested.y);
    ASSERT_TRUE(RemoveWindowSubclass(hwnd, inject_dpi_on_move, 1));
    RECT actual = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &actual));
    ASSERT_EQ(injection.submissions, 4);
    ASSERT_EQ(injection.max_depth, 1);
    ASSERT_EQ(geometry_count, 1);
    ASSERT_EQ(position_count, 1);
    ASSERT_EQ(saved.x, actual.left);
    ASSERT_EQ(saved.y, actual.top);
    ASSERT_EQ(actual.right, info.rcWork.right);
    ASSERT_EQ(actual.bottom, info.rcWork.bottom);
    ASSERT_TRUE(actual.right - actual.left > initial.right - initial.left);
    // There must be no pending DPI correction left for the next movement.
    window.set_position(actual.left, actual.top);
    ASSERT_EQ(geometry_count, 1);
    ASSERT_EQ(position_count, 1);
}

TEST(StatusWindow, ReleaseUsesPointerMonitorInsteadOfWindowMajority) {
    test::ScopedDpiAwarenessContext dpi_context;
    const std::vector<HMONITOR> monitors = monitor_handles();
    if (monitors.size() < 2) {
        return;
    }
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));
    const HWND hwnd = window.hwnd_for_test();
    MONITORINFO source = {sizeof(source)};
    MONITORINFO target = {sizeof(target)};
    ASSERT_TRUE(GetMonitorInfoW(monitors[0], &source));
    ASSERT_TRUE(GetMonitorInfoW(monitors[1], &target));
    window.set_position(source.rcWork.left, source.rcWork.top);
    const POINT anchor = status_button_center(hwnd, 0);
    send_mouse_at_screen_point(hwnd, WM_LBUTTONDOWN,
                               {source.rcWork.left + anchor.x, source.rcWork.top + anchor.y});
    send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE,
                               {source.rcWork.left + anchor.x + 40, source.rcWork.top + anchor.y});
    // The release event can contain a newer pointer position than the last move event.
    const POINT release = {target.rcWork.left + (target.rcWork.right - target.rcWork.left) / 2,
                           target.rcWork.top + (target.rcWork.bottom - target.rcWork.top) / 2};
    send_mouse_at_screen_point(hwnd, WM_LBUTTONUP, release);
    RECT actual = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &actual));
    ASSERT_TRUE(actual.left >= target.rcWork.left && actual.right <= target.rcWork.right);
    ASSERT_TRUE(actual.top >= target.rcWork.top && actual.bottom <= target.rcWork.bottom);
}

TEST(StatusWindow, DpiFallbackStaysOnTargetUntilPointerChangesScreens) {
    test::ScopedDpiAwarenessContext dpi_context;
    const std::vector<HMONITOR> monitors = monitor_handles();
    // This integration test needs an ordinary vertical seam with room for the window.
    for (HMONITOR left_handle : monitors) {
        MONITORINFO left = {sizeof(left)};
        ASSERT_TRUE(GetMonitorInfoW(left_handle, &left));
        for (HMONITOR right_handle : monitors) {
            MONITORINFO right = {sizeof(right)};
            ASSERT_TRUE(GetMonitorInfoW(right_handle, &right));
            if (left.rcMonitor.right != right.rcMonitor.left ||
                left.rcWork.right != right.rcWork.left) {
                continue;
            }
            cxxime::StatusWindow window;
            ASSERT_TRUE(create_test_window(window));
            window.set_position(left.rcWork.left, left.rcWork.top);
            const HWND hwnd = window.hwnd_for_test();
            RECT rect = {};
            ASSERT_TRUE(GetWindowRect(hwnd, &rect));
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            const int top = (std::max)(left.rcWork.top, right.rcWork.top);
            const int bottom = (std::min)(left.rcWork.bottom, right.rcWork.bottom);
            if (bottom - top < height * 4 || left.rcWork.right - left.rcWork.left < width * 2) {
                continue;
            }
            const int seam = left.rcWork.right;
            const int y = top + (bottom - top) / 2;
            window.set_position(seam - width * 2, y);
            const POINT anchor = status_button_center(hwnd, 3);
            send_mouse_at_screen_point(hwnd, WM_LBUTTONDOWN,
                                       {seam - width * 2 + anchor.x, y + anchor.y});
            const UINT dpi = window.dpi();
            DpiInjection injection = {{dpi + 24, dpi, dpi + 48}};
            ASSERT_TRUE(SetWindowSubclass(hwnd, inject_dpi_on_move, 1,
                                          reinterpret_cast<DWORD_PTR>(&injection)));
            const POINT on_right = {seam + 1, y + anchor.y};
            send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE, on_right);
            ASSERT_TRUE(RemoveWindowSubclass(hwnd, inject_dpi_on_move, 1));
            ASSERT_EQ(injection.max_depth, 1);
            // Repeating the same raw cross-screen request must not pull it back again.
            send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE, on_right);
            ASSERT_TRUE(GetWindowRect(hwnd, &rect));
            ASSERT_TRUE(rect.left >= right.rcWork.left && rect.right <= right.rcWork.right);

            const POINT on_left = {seam - 1, y + anchor.y};
            send_mouse_at_screen_point(hwnd, WM_MOUSEMOVE, on_left);
            ASSERT_TRUE(GetWindowRect(hwnd, &rect));
            ASSERT_TRUE(rect.left < seam && rect.right > seam);
            send_mouse_at_screen_point(hwnd, WM_LBUTTONUP, on_left);
            return;
        }
    }
}

TEST(StatusWindow, DpiChangeUsesRenderedSizeAndNotifiesGeometryOnce) {
    test::ScopedDpiAwarenessContext dpi_context;
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));
    const HWND hwnd = window.hwnd_for_test();
    MONITORINFO info = {sizeof(info)};
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info));
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &rect));
    SendMessageW(hwnd, WM_DPICHANGED, MAKELPARAM(96, 96), reinterpret_cast<LPARAM>(&rect));
    ASSERT_TRUE(GetWindowRect(hwnd, &rect));
    int callback_count = 0;
    window.set_geometry_changed_callback([&]() { ++callback_count; });
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const UINT old_dpi = 96;
    const UINT next_dpi = 120;
    // Windows scales the whole rect; the layout rounds its individual metrics.
    const int suggested_width = MulDiv(width, next_dpi, old_dpi);
    const int suggested_height = MulDiv(height, next_dpi, old_dpi);
    RECT suggested = {info.rcWork.right - suggested_width, info.rcWork.bottom - suggested_height,
                      info.rcWork.right, info.rcWork.bottom};
    SendMessageW(hwnd, WM_DPICHANGED, MAKELPARAM(next_dpi, next_dpi),
                 reinterpret_cast<LPARAM>(&suggested));
    ASSERT_TRUE(GetWindowRect(hwnd, &rect));
    ASSERT_TRUE(rect.right <= info.rcWork.right);
    ASSERT_TRUE(rect.bottom <= info.rcWork.bottom);
    ASSERT_TRUE(rect.right - rect.left != suggested_width);
    ASSERT_EQ(callback_count, 1);
}

// ============================================================
// Callbacks
// ============================================================

TEST(StatusWindow, ClickCallback) {
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));

    int click_count = 0;
    cxxime::StatusButton last_button = cxxime::StatusButton::SETTINGS;
    window.set_click_callback([&](cxxime::StatusButton btn) {
        click_count++;
        last_button = btn;
    });

    window.show();

    const POINT point = status_button_center(window.hwnd_for_test(), 0);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0, MAKELPARAM(point.x, point.y));
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));

    ASSERT_EQ(click_count, 1);
    ASSERT_TRUE(last_button == cxxime::StatusButton::CHINESE_MODE);

    window.destroy();
}

TEST(StatusWindow, ClickWhenDisabled) {
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));

    int click_count = 0;
    window.set_click_callback([&](cxxime::StatusButton) { click_count++; });

    window.set_enabled(false);

    const POINT point = status_button_center(window.hwnd_for_test(), 0);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0, MAKELPARAM(point.x, point.y));
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));

    ASSERT_EQ(click_count, 0);

    window.destroy();
}

// ============================================================
// Input mode is non-interactive
// ============================================================

TEST(StatusWindow, InputModeClickIgnored) {
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));

    int click_count = 0;
    window.set_click_callback([&](cxxime::StatusButton) { click_count++; });

    window.show();

    const POINT point = input_mode_center(window.hwnd_for_test());
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0, MAKELPARAM(point.x, point.y));
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));

    ASSERT_EQ(click_count, 0);

    window.destroy();
}

// ============================================================
// Settings button
// ============================================================

TEST(StatusWindow, SettingsClick) {
    cxxime::StatusWindow window;
    ASSERT_TRUE(create_test_window(window));

    cxxime::StatusButton last_button = cxxime::StatusButton::CHINESE_MODE;
    window.set_click_callback([&](cxxime::StatusButton btn) {
        last_button = btn;
    });

    window.show();

    const POINT point = status_button_center(window.hwnd_for_test(), 3);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0, MAKELPARAM(point.x, point.y));
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));

    ASSERT_TRUE(last_button == cxxime::StatusButton::SETTINGS);

    window.destroy();
}

RUN_ALL_TESTS()
