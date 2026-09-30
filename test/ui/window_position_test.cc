// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <vector>

#include <windows.h>

#include <cxxime/window_position.h>

#include "support/dpi_testutil.h"
#include "support/testutil.h"

TEST(WindowPosition, transforms_caret_with_valid_anchor_and_outside_end) {
    test::ScopedDpiAwarenessContext dpi_context;
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(window != nullptr);

    RECT client = {};
    ASSERT_TRUE(GetClientRect(window, &client) != FALSE);
    POINT top_left = {client.left, client.top};
    POINT bottom_right = {client.right, client.bottom};
    ASSERT_TRUE(ClientToScreen(window, &top_left) != FALSE);
    ASSERT_TRUE(ClientToScreen(window, &bottom_right) != FALSE);

    RECT source = {bottom_right.x - 1, bottom_right.y - 1, bottom_right.x, bottom_right.y + 20};
    RECT transformed = {};
    ASSERT_TRUE(cxxime::logical_screen_rect_to_physical(window, source, &transformed));
    ASSERT_EQ(source.left, transformed.left);
    ASSERT_EQ(source.top, transformed.top);
    ASSERT_EQ(source.right, transformed.right);
    ASSERT_EQ(source.bottom, transformed.bottom);

    DestroyWindow(window);
}

TEST(WindowPosition, rejects_invalid_window) {
    RECT source = {10, 10, 11, 30};
    RECT transformed = {};
    ASSERT_TRUE(!cxxime::logical_screen_rect_to_physical(nullptr, source, &transformed));
}

TEST(WindowPosition, candidate_prefers_below_across_reserved_work_area) {
    constexpr LONG kReservedWorkAreaBottom = 760;
    const RECT monitor = {0, 0, 1000, 800};
    const RECT caret = {100, 700, 101, 720};

    const auto placement = cxxime::calculate_candidate_window_position(
        caret, 300, 70, 4, monitor, cxxime::CandidatePlacementSide::Unset);

    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(placement.position.x, 100);
    ASSERT_EQ(placement.position.y, 724);
    ASSERT_GT(placement.position.y + 70, kReservedWorkAreaBottom);
}

TEST(WindowPosition, candidate_side_does_not_oscillate_at_physical_boundary) {
    const RECT monitor = {0, 0, 1632, 1368};
    RECT caret = {240, 1258, 241, 1278};

    auto placement = cxxime::calculate_candidate_window_position(
        caret, 663, 86, 4, monitor, cxxime::CandidatePlacementSide::Unset);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(placement.position.y, 1282);

    ++caret.top;
    ++caret.bottom;
    placement =
        cxxime::calculate_candidate_window_position(caret, 663, 86, 4, monitor, placement.side);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(placement.position.y, 1169);

    --caret.top;
    --caret.bottom;
    placement =
        cxxime::calculate_candidate_window_position(caret, 663, 86, 4, monitor, placement.side);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(placement.position.y, 1168);
}

TEST(WindowPosition, candidate_height_shrink_preserves_above_side) {
    const RECT monitor = {0, 0, 1632, 1368};
    const RECT caret = {240, 1145, 241, 1165};

    auto placement = cxxime::calculate_candidate_window_position(
        caret, 162, 51, 4, monitor, cxxime::CandidatePlacementSide::Unset);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Below);

    placement =
        cxxime::calculate_candidate_window_position(caret, 162, 267, 4, monitor, placement.side);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(placement.position.y, 874);

    placement =
        cxxime::calculate_candidate_window_position(caret, 162, 51, 4, monitor, placement.side);
    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(placement.position.y, 1090);
}

TEST(WindowPosition, candidate_switches_side_to_remain_visible) {
    const RECT monitor = {0, 0, 1000, 800};
    const RECT caret = {100, 20, 101, 40};

    const auto placement = cxxime::calculate_candidate_window_position(
        caret, 300, 100, 4, monitor, cxxime::CandidatePlacementSide::Above);

    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(placement.position.y, 44);
}

TEST(WindowPosition, candidate_keeps_side_when_neither_side_fits) {
    const RECT monitor = {0, 0, 1000, 200};
    const RECT caret = {100, 90, 101, 110};

    const auto placement = cxxime::calculate_candidate_window_position(
        caret, 300, 150, 4, monitor, cxxime::CandidatePlacementSide::Above);

    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(placement.position.y, 0);

    const auto initial = cxxime::calculate_candidate_window_position(
        caret, 300, 150, 4, monitor, cxxime::CandidatePlacementSide::Unset);
    ASSERT_EQ(initial.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(initial.position.y, 50);
}

TEST(WindowPosition, candidate_clamps_to_negative_monitor_coordinates) {
    const RECT monitor = {-1920, 0, 0, 1080};
    const RECT caret = {-20, 900, -19, 920};

    const auto placement = cxxime::calculate_candidate_window_position(
        caret, 400, 100, 4, monitor, cxxime::CandidatePlacementSide::Unset);

    ASSERT_EQ(placement.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(placement.position.x, -400);
    ASSERT_EQ(placement.position.y, 924);
}

TEST(WindowPosition, transforms_caret_when_only_end_anchor_is_inside) {
    test::ScopedDpiAwarenessContext dpi_context;
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(window != nullptr);

    RECT client = {};
    ASSERT_TRUE(GetClientRect(window, &client) != FALSE);
    POINT top_left = {client.left, client.top};
    ASSERT_TRUE(ClientToScreen(window, &top_left) != FALSE);

    RECT source = {top_left.x - 20, top_left.y, top_left.x + 1, top_left.y + 20};
    RECT transformed = {};
    ASSERT_TRUE(cxxime::logical_screen_rect_to_physical(window, source, &transformed));
    ASSERT_EQ(source.left, transformed.left);
    ASSERT_EQ(source.top, transformed.top);
    ASSERT_EQ(source.right, transformed.right);
    ASSERT_EQ(source.bottom, transformed.bottom);

    DestroyWindow(window);
}

TEST(WindowPosition, transforms_rect_intersecting_client_without_inside_corners) {
    test::ScopedDpiAwarenessContext dpi_context;
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(window != nullptr);

    RECT client = {};
    ASSERT_TRUE(GetClientRect(window, &client) != FALSE);
    POINT top_left = {client.left, client.top};
    POINT bottom_right = {client.right, client.bottom};
    ASSERT_TRUE(ClientToScreen(window, &top_left) != FALSE);
    ASSERT_TRUE(ClientToScreen(window, &bottom_right) != FALSE);

    const LONG center_y = top_left.y + (bottom_right.y - top_left.y) / 2;
    RECT source = {top_left.x - 20, center_y - 10, bottom_right.x + 20, center_y + 10};
    RECT transformed = {};
    ASSERT_TRUE(cxxime::logical_screen_rect_to_physical(window, source, &transformed));
    ASSERT_EQ(source.left, transformed.left);
    ASSERT_EQ(source.top, transformed.top);
    ASSERT_EQ(source.right, transformed.right);
    ASSERT_EQ(source.bottom, transformed.bottom);

    DestroyWindow(window);
}

TEST(WindowPosition, SingleMonitorClampsOrdinaryEdgesButMultipleMonitorsAllowCrossing) {
    constexpr int kWindowWidth = 200;
    constexpr int kWindowHeight = 40;
    const MONITORINFO monitor = {
        sizeof(MONITORINFO), {-1920, -1080, 0, 0}, {-1920, -1080, 0, -40}, 0};
    const MONITORINFO neighbor = {sizeof(MONITORINFO), {0, -1080, 1920, 0}, {0, -1080, 1920, 0}, 0};
    const POINT single = cxxime::constrain_status_drag_position(
        -100, -500, kWindowWidth, kWindowHeight, {monitor}, monitor.rcWork);
    ASSERT_EQ(single.x, -kWindowWidth);
    ASSERT_EQ(single.y, -500);
    const POINT multiple = cxxime::constrain_status_drag_position(
        -100, -500, kWindowWidth, kWindowHeight, {monitor, neighbor}, monitor.rcWork);
    ASSERT_EQ(multiple.x, -100);
    ASSERT_EQ(multiple.y, -500);
}

TEST(WindowPosition, MultipleMonitorsConstrainOnlyReservedEdges) {
    constexpr int kWindowWidth = 200;
    constexpr int kWindowHeight = 40;
    constexpr int kTaskbarThickness = 40;
    // A screen above and to the left of the primary screen exercises negative coordinates.
    const RECT monitor = {-1920, -1080, 0, 0};
    const MONITORINFO primary_monitor = {
        sizeof(MONITORINFO), {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, MONITORINFOF_PRIMARY};
    const RECT bottom_work = {monitor.left, monitor.top, monitor.right,
                              monitor.bottom - kTaskbarThickness};
    const RECT top_work = {monitor.left, monitor.top + kTaskbarThickness, monitor.right,
                           monitor.bottom};
    const RECT left_work = {monitor.left + kTaskbarThickness, monitor.top, monitor.right,
                            monitor.bottom};
    const RECT right_work = {monitor.left, monitor.top, monitor.right - kTaskbarThickness,
                             monitor.bottom};
    struct Case {
        RECT work;
        POINT requested;
        POINT expected;
    };
    const Case cases[] = {
        // Bottom, top, left and right taskbars. The other axis stays free.
        {bottom_work, {-1000, -60}, {-1000, -80}},
        {top_work, {-1000, -1060}, {-1000, -1040}},
        {left_work, {-1900, -500}, {-1880, -500}},
        {right_work, {-100, -500}, {-240, -500}},
        // Ordinary edges allow a window to straddle monitors.
        {bottom_work, {-2000, -500}, {-2000, -500}},
        {bottom_work, {-100, -60}, {-100, -80}},
        // No reservation, or no intersection with this monitor.
        {monitor, {-100, -20}, {-100, -20}},
        {bottom_work, {0, -20}, {0, -20}},
        {bottom_work, {-1000, 0}, {-1000, 0}},
        // Touching the work-area boundary needs no correction.
        {bottom_work, {-1000, -80}, {-1000, -80}},
    };
    for (const Case& item : cases) {
        const POINT result = cxxime::constrain_status_drag_position(
            item.requested.x, item.requested.y, kWindowWidth, kWindowHeight,
            {{sizeof(MONITORINFO), monitor, item.work, 0}, primary_monitor}, item.work);
        ASSERT_EQ(result.x, item.expected.x);
        ASSERT_EQ(result.y, item.expected.y);
    }
}

TEST(WindowPosition, CrossMonitorPlacementAvoidsTheOtherMonitorsTaskbar) {
    constexpr int kWindowWidth = 200;
    constexpr int kWindowHeight = 40;
    constexpr int kTaskbarThickness = 40;
    constexpr int kNeighborOverlap = 20;
    const RECT left_monitor = {-1920, 0, 0, 1080};
    const RECT right_monitor = {0, 0, 1920, 1080};
    const RECT right_work = {right_monitor.left, right_monitor.top, right_monitor.right,
                             right_monitor.bottom - kTaskbarThickness};
    const std::vector<MONITORINFO> monitors = {
        {sizeof(MONITORINFO), left_monitor, left_monitor, 0},
        {sizeof(MONITORINFO), right_monitor, right_work, 0},
    };
    // Most of the window is on the left, but 20 pixels overlap the right screen.
    // Its bottom (1070) crosses the taskbar boundary (1040) on that screen.
    const POINT requested = {-kWindowWidth + kNeighborOverlap, 1030};
    POINT result = cxxime::constrain_status_drag_position(requested.x, requested.y, kWindowWidth,
                                                          kWindowHeight, monitors, left_monitor);
    ASSERT_EQ(result.x, -180);
    ASSERT_EQ(result.y, 1000);

    // The same boundary can be crossed freely once the whole window is on the left.
    result = cxxime::constrain_status_drag_position(-kWindowWidth, requested.y, kWindowWidth,
                                                    kWindowHeight, monitors, left_monitor);
    ASSERT_EQ(result.x, -200);
    ASSERT_EQ(result.y, 1030);
}

TEST(WindowPosition, ConflictingReservedEdgesResolveWithinOneWorkArea) {
    constexpr int kWindowWidth = 200;
    constexpr int kWindowHeight = 80;
    constexpr int kTaskbarThickness = 40;
    // Vertically offset adjacent screens: moving away from either taskbar can
    // overlap the other one. A single pass alone would depend on enumeration order.
    const RECT left_monitor = {0, 0, 1920, 1080};
    const RECT right_monitor = {1920, 1000, 3840, 2080};
    const MONITORINFO left = {sizeof(MONITORINFO),
                              left_monitor,
                              {left_monitor.left, left_monitor.top, left_monitor.right,
                               left_monitor.bottom - kTaskbarThickness},
                              0};
    const MONITORINFO right = {sizeof(MONITORINFO),
                               right_monitor,
                               {right_monitor.left, right_monitor.top + kTaskbarThickness,
                                right_monitor.right, right_monitor.bottom},
                               0};
    // Straddle x=1920 and both taskbars: left y=[1040,1080), right y=[1000,1040).
    const POINT requested = {1850, 1020};
    const MONITORINFO targets[] = {left, right};
    const POINT expected[] = {{1720, 960}, {1920, 1040}};
    for (int i = 0; i < 2; ++i) {
        const POINT result =
            cxxime::constrain_status_drag_position(requested.x, requested.y, kWindowWidth,
                                                   kWindowHeight, {left, right}, targets[i].rcWork);
        ASSERT_EQ(result.x, expected[i].x);
        ASSERT_EQ(result.y, expected[i].y);
        const POINT reversed =
            cxxime::constrain_status_drag_position(requested.x, requested.y, kWindowWidth,
                                                   kWindowHeight, {right, left}, targets[i].rcWork);
        ASSERT_EQ(reversed.x, result.x);
        ASSERT_EQ(reversed.y, result.y);
        const POINT repeated = cxxime::constrain_status_drag_position(
            result.x, result.y, kWindowWidth, kWindowHeight, {left, right}, targets[i].rcWork);
        ASSERT_EQ(repeated.x, result.x);
        ASSERT_EQ(repeated.y, result.y);
    }
}

RUN_ALL_TESTS()
