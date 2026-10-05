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

RUN_ALL_TESTS()
