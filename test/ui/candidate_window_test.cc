// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <windows.h>

#include <cxxime/candidate_window.h>
#include <cxxime/config.h>
#include <cxxime/status_window.h>

#include "support/dpi_testutil.h"
#include "support/testutil.h"

namespace {

bool window_is_above(HWND upper, HWND lower) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == upper) {
            return true;
        }
        if (window == lower) {
            return false;
        }
    }
    return false;
}

} // namespace

TEST(CandidateWindow, page_buttons_use_page_callback) {
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));

    int callback_count = 0;
    int selection_count = 0;
    std::size_t selected_index = 1;
    cxxime::CandidatePageDirection last_direction = cxxime::CandidatePageDirection::Previous;
    window.set_candidate_selection_callback([&](std::size_t index) {
        ++selection_count;
        selected_index = index;
    });
    window.set_page_callback([&](cxxime::CandidatePageDirection direction) {
        ++callback_count;
        last_direction = direction;
    });

    window.set_page_info(1, 2);
    window.update(page);
    RECT previous = window.page_button_rect_for_test(cxxime::CandidatePageDirection::Previous);
    ASSERT_TRUE(previous.right > previous.left);
    SendMessageW(
        window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
        MAKELPARAM((previous.left + previous.right) / 2,
                   (previous.top + previous.bottom) / 2));
    ASSERT_EQ(callback_count, 0);
    ASSERT_EQ(selection_count, 0);

    RECT next = window.page_button_rect_for_test(cxxime::CandidatePageDirection::Next);
    ASSERT_TRUE(next.right > next.left);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
                 MAKELPARAM((next.left + next.right) / 2, (next.top + next.bottom) / 2));
    ASSERT_EQ(callback_count, 1);
    ASSERT_EQ(selection_count, 0);
    ASSERT_EQ(last_direction, cxxime::CandidatePageDirection::Next);

    window.set_page_info(2, 2);
    window.update(page);
    previous = window.page_button_rect_for_test(cxxime::CandidatePageDirection::Previous);
    ASSERT_TRUE(previous.right > previous.left);
    SendMessageW(
        window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
        MAKELPARAM((previous.left + previous.right) / 2, (previous.top + previous.bottom) / 2));
    ASSERT_EQ(callback_count, 2);
    ASSERT_EQ(selection_count, 0);
    ASSERT_EQ(last_direction, cxxime::CandidatePageDirection::Previous);

    next = window.page_button_rect_for_test(cxxime::CandidatePageDirection::Next);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
                 MAKELPARAM((next.left + next.right) / 2, (next.top + next.bottom) / 2));
    ASSERT_EQ(callback_count, 2);
    ASSERT_EQ(selection_count, 0);

    RECT candidate_rect = window.candidate_rect_for_test(0);
    ASSERT_TRUE(candidate_rect.right > candidate_rect.left);
    SendMessageW(
        window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
        MAKELPARAM((candidate_rect.left + candidate_rect.right) / 2,
                   (candidate_rect.top + candidate_rect.bottom) / 2));
    ASSERT_EQ(callback_count, 2);
    ASSERT_EQ(selection_count, 1);
    ASSERT_EQ(selected_index, 0);

    window.destroy();
}

TEST(CandidateWindow, visible_update_keeps_window_visible_after_show) {
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidatePage page;
    page.candidates.push_back(cxxime::Candidate{"candidate", "hint"});

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);
    window.show();
    ASSERT_TRUE(window.is_visible());

    page.candidates[0].text = "updated";
    window.update(page);
    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 1);

    page.candidates[0].text = "hidden-between-update-and-show";
    window.update(page);
    ShowWindow(window.hwnd_for_test(), SW_HIDE);
    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 1);
    window.destroy();
}

TEST(CandidateWindow, first_show_after_update_still_paints_and_shows) {
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidatePage page;
    page.candidates.push_back(cxxime::Candidate{"candidate", "hint"});

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);
    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 1);
    window.destroy();
}

TEST(CandidateWindow, hide_then_show_keeps_first_show_redraw_semantics) {
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidatePage page;
    page.candidates.push_back(cxxime::Candidate{"candidate", "hint"});

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);
    window.show();
    window.hide();
    ASSERT_TRUE(!window.is_visible());

    page.candidates[0].text = "after-hide";
    window.update(page);
    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 1);
    window.destroy();
}

TEST(CandidateWindow, preedit_and_page_info_changes_do_not_skip_next_show) {
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidatePage page;
    page.candidates.push_back(cxxime::Candidate{"candidate", "hint"});

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);
    window.show();

    window.update(page);
    window.set_preedit("nihao");
    window.show();
    ASSERT_TRUE(window.is_visible());

    window.update(page);
    window.set_page_info(2, 2);
    window.show();
    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 1);
    window.destroy();
}

TEST(CandidateWindow, indeterminate_extent_keeps_next_page_available) {
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidatePage page;
    for (int index = 0; index < 4; ++index) {
        cxxime::Candidate candidate;
        candidate.text = "candidate" + std::to_string(index);
        page.candidates.push_back(std::move(candidate));
    }
    page.extent.known_count = 4;
    page.extent.state = cxxime::CandidateExtentState::kIndeterminate;
    page.extent.complete = false;

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    int callback_count = 0;
    window.set_page_callback([&](cxxime::CandidatePageDirection direction) {
        ASSERT_EQ(direction, cxxime::CandidatePageDirection::Next);
        ++callback_count;
    });
    window.set_page_info(1, 1);
    window.update(page);

    const RECT next = window.page_button_rect_for_test(cxxime::CandidatePageDirection::Next);
    ASSERT_TRUE(next.right > next.left);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, 0,
                 MAKELPARAM((next.left + next.right) / 2, (next.top + next.bottom) / 2));
    ASSERT_EQ(callback_count, 1);
    window.destroy();
}

TEST(CandidateWindow, recreate_resets_native_window_size_cache) {
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);

    HWND first_hwnd = window.hwnd_for_test();
    ASSERT_TRUE(first_hwnd != nullptr);
    RECT first_rect = {};
    ASSERT_TRUE(GetWindowRect(first_hwnd, &first_rect) != FALSE);
    window.destroy();

    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);

    HWND second_hwnd = window.hwnd_for_test();
    ASSERT_TRUE(second_hwnd != nullptr);
    RECT second_rect = {};
    ASSERT_TRUE(GetWindowRect(second_hwnd, &second_rect) != FALSE);

    ASSERT_EQ(second_rect.right - second_rect.left, first_rect.right - first_rect.left);
    ASSERT_EQ(second_rect.bottom - second_rect.top, first_rect.bottom - first_rect.top);
    window.destroy();
}

TEST(CandidateWindow, candidate_to_preedit_only_clears_candidate_layout) {
    cxxime::Config config;
    config.render_backend = "d2d";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.set_preedit("hs");
    window.update(page);
    window.show();
    ASSERT_TRUE(window.visible_candidate_count() > 0);

    window.set_preedit("hse", 3, 0, 0, 3, false);
    window.update(cxxime::CandidatePage{});
    window.show();

    ASSERT_TRUE(window.is_visible());
    ASSERT_EQ(window.visible_candidate_count(), 0);
    const RECT active = window.preedit_active_rect_for_test();
    ASSERT_GT(active.right, active.left);
    ASSERT_GT(active.bottom, active.top);
    const RECT stale_candidate = window.candidate_rect_for_test(0);
    ASSERT_EQ(stale_candidate.right - stale_candidate.left, 0);
    ASSERT_EQ(stale_candidate.bottom - stale_candidate.top, 0);
    window.destroy();
}

TEST(CandidateWindow, focused_syllable_geometry_keeps_separator_outside) {
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    const std::string converted = u8"华锐";
    const std::string preedit = converted + "ji'shu";
    window.set_preedit(preedit, converted.size(), converted.size(), converted.size(),
                       converted.size() + 2, true);
    window.update(page);

    const RECT active = window.preedit_active_rect_for_test();
    const RECT cursor = window.preedit_cursor_rect_for_test();
    ASSERT_TRUE(active.right > active.left);
    ASSERT_GT(cursor.left, active.left);
    const int minimum_cursor_width =
        (std::max)(1, MulDiv(2, static_cast<int>(window.dpi()), 96));
    ASSERT_GE(cursor.right - cursor.left, minimum_cursor_width);
    ASSERT_LT(cursor.bottom - cursor.top, active.bottom - active.top);
    ASSERT_TRUE(window.preedit_cursor_in_focus_for_test());
    ASSERT_TRUE(!window.preedit_cursor_emphasized_for_test());
    const cxxime::Theme cursor_theme = cxxime::build_theme_from_config(config);
    const cxxime::Color idle_cursor = window.preedit_cursor_idle_for_test();
    ASSERT_TRUE(idle_cursor.r != cursor_theme.preedit_text.r ||
                idle_cursor.g != cursor_theme.preedit_text.g ||
                idle_cursor.b != cursor_theme.preedit_text.b);
    ASSERT_TRUE(idle_cursor.r != cursor_theme.preedit_cursor.r ||
                idle_cursor.g != cursor_theme.preedit_cursor.g ||
                idle_cursor.b != cursor_theme.preedit_cursor.b);
    bool found_separator = false;
    for (const auto& run : window.preedit_runs_for_test()) {
        if (run.kind == cxxime::PreeditRunKind::Separator) {
            ASSERT_GT(run.rect.left, active.right);
            ASSERT_TRUE(!run.focused);
            found_separator = true;
            break;
        }
    }
    ASSERT_TRUE(found_separator);

    window.set_preedit("abcd", 4, 0, 4, 4, false);
    window.update(page);
    const RECT unfocused = window.preedit_active_rect_for_test();
    ASSERT_EQ(unfocused.right - unfocused.left, 0);
    const RECT end_cursor = window.preedit_cursor_rect_for_test();
    ASSERT_EQ(end_cursor.right - end_cursor.left, 0);
    ASSERT_EQ(end_cursor.bottom - end_cursor.top, 0);

    window.set_preedit("don't", 5, 0, 5, 5, false);
    window.update(page);
    ASSERT_EQ(window.preedit_runs_for_test().size(), static_cast<std::size_t>(1));
    ASSERT_EQ(window.preedit_runs_for_test()[0].text, std::string("don't"));
    window.destroy();
}

TEST(CandidateWindow, preedit_highlight_uses_advanced_layout_metrics) {
    cxxime::Config config;
    config.render_backend = "gdi";
    config.layout_config.preedit_highlight_padding_x = 9;
    config.layout_config.preedit_highlight_padding_y = 5;
    config.layout_config.preedit_boundary_gap = 7;
    config.layout_config.preedit_highlight_corner = 11;
    config.layout_config.preedit_highlight_border_width = 3;

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.set_preedit("ji'shu", 0, 0, 0, 2, true);
    window.update(page);

    const float scale = window.dpi() / 96.0f;
    const int expected_padding_x = static_cast<int>(9 * scale);
    const int expected_padding_y = static_cast<int>(5 * scale);
    const int expected_boundary_gap = static_cast<int>(7 * scale);
    const RECT active = window.preedit_active_rect_for_test();
    const RECT cursor = window.preedit_cursor_rect_for_test();
    const int minimum_cursor_width =
        (std::max)(1, MulDiv(2, static_cast<int>(window.dpi()), 96));
    ASSERT_GE(cursor.right - cursor.left, minimum_cursor_width);
    const int text_height = active.bottom - active.top - expected_padding_y * 2;
    const int expected_cursor_height = (std::max)(1, (text_height * 80 + 50) / 100);
    ASSERT_EQ(cursor.bottom - cursor.top, expected_cursor_height);
    const int border_gap = (std::max)(
        1, MulDiv(1, static_cast<int>(window.dpi()), 96));
    const int safe_inset = (window.preedit_border_width_for_test() + 1) / 2 + border_gap;
    const int horizontal_safe_inset =
        safe_inset + window.preedit_corner_radius_for_test();
    const int text_gap = (std::max)(
        1, MulDiv(1, static_cast<int>(window.dpi()), 96));
    ASSERT_EQ(cursor.left - active.left,
              (std::max)(expected_padding_x, horizontal_safe_inset) + text_gap);
    ASSERT_GE(cursor.top, active.top + safe_inset);
    ASSERT_LE(cursor.bottom, active.bottom - safe_inset);
    ASSERT_TRUE(window.preedit_cursor_in_focus_for_test());
    ASSERT_TRUE(!window.preedit_cursor_emphasized_for_test());
    ASSERT_EQ(window.preedit_corner_radius_for_test(), static_cast<int>(11 * scale));
    ASSERT_EQ(window.preedit_border_width_for_test(), static_cast<int>(3 * scale));

    bool found_focused = false;
    bool found_separator = false;
    for (const auto& run : window.preedit_runs_for_test()) {
        if (run.focused) {
            ASSERT_EQ(run.rect.top - active.top, expected_padding_y);
            found_focused = true;
        } else if (run.kind == cxxime::PreeditRunKind::Separator) {
            ASSERT_EQ(run.rect.left - active.right, expected_boundary_gap);
            found_separator = true;
        }
    }
    ASSERT_TRUE(found_focused);
    ASSERT_TRUE(found_separator);

    config.layout_config.max_width = 160;
    config.layout_config.preedit_highlight_padding_x = 20;
    window.set_config(config);
    const std::string long_preedit(128, 'w');
    window.set_preedit(long_preedit, long_preedit.size(), 0, 0, long_preedit.size(), false);
    window.update(page);
    const SIZE constrained = window.layout_size();
    ASSERT_LE(window.preedit_active_rect_for_test().right, constrained.cx);
    ASSERT_EQ(window.preedit_cursor_rect_for_test().right -
                  window.preedit_cursor_rect_for_test().left,
              0);
    for (const auto& run : window.preedit_runs_for_test()) {
        ASSERT_LE(run.rect.right, constrained.cx);
    }

    window.set_preedit(long_preedit, 64, 0, 0, long_preedit.size(), false);
    window.update(page);
    ASSERT_EQ(window.preedit_cursor_rect_for_test().right -
                  window.preedit_cursor_rect_for_test().left,
              0);
    ASSERT_LE(window.preedit_cursor_rect_for_test().right, window.layout_size().cx);
    ASSERT_TRUE(window.preedit_cursor_in_focus_for_test());

    window.set_preedit(long_preedit, 5, 0, 0, long_preedit.size(), false);
    window.update(page);
    const RECT near_edge_cursor = window.preedit_cursor_rect_for_test();
    ASSERT_GT(near_edge_cursor.right, near_edge_cursor.left);
    const auto& near_edge_runs = window.preedit_runs_for_test();
    const std::string near_edge_suffix = long_preedit.substr(5);
    const auto near_edge_run =
        std::find_if(near_edge_runs.begin(), near_edge_runs.end(), [&](const auto& run) {
            return run.text == near_edge_suffix;
        });
    ASSERT_TRUE(near_edge_run != near_edge_runs.end());
    ASSERT_LE(near_edge_cursor.right + text_gap, near_edge_run->rect.left);

    window.set_preedit("ji'shu", 3, 0, 0, 2, true);
    window.update(page);
    ASSERT_GE(window.preedit_cursor_rect_for_test().right -
                  window.preedit_cursor_rect_for_test().left,
              minimum_cursor_width);
    ASSERT_TRUE(!window.preedit_cursor_in_focus_for_test());

    window.set_preedit("ji'shu", 1, 0, 0, 2, true);
    window.update(page);
    ASSERT_TRUE(window.preedit_cursor_in_focus_for_test());
    ASSERT_TRUE(window.preedit_cursor_emphasized_for_test());

    int emphasis_layout_callback_count = 0;
    window.set_layout_changed_callback(
        [&emphasis_layout_callback_count]() { ++emphasis_layout_callback_count; });
    constexpr WPARAM kPreeditCursorEmphasisTimerId = 1;
    SendMessageW(window.hwnd_for_test(), WM_TIMER, kPreeditCursorEmphasisTimerId, 0);
    ASSERT_TRUE(!window.preedit_cursor_emphasized_for_test());
    ASSERT_EQ(emphasis_layout_callback_count, 0);

    window.set_preedit("ji'shu", 0, 0, 0, 2, true);
    window.update(page);
    ASSERT_TRUE(window.preedit_cursor_emphasized_for_test());
    window.set_preedit("ji'shux", 1, 0, 0, 2, true);
    window.update(page);
    ASSERT_TRUE(!window.preedit_cursor_emphasized_for_test());

    config.layout_config.max_width = 0;
    config.layout_config.preedit_highlight_padding_x = 0;
    config.layout_config.preedit_highlight_padding_y = 0;
    config.layout_config.preedit_highlight_border_width = 3;
    window.set_config(config);
    window.set_preedit("ji'shu", 0, 0, 0, 2, true);
    window.update(page);
    const RECT tight_active = window.preedit_active_rect_for_test();
    const RECT start_cursor = window.preedit_cursor_rect_for_test();
    const int tight_safe_inset =
        (window.preedit_border_width_for_test() + 1) / 2 + border_gap;
    const int tight_horizontal_safe_inset =
        tight_safe_inset + window.preedit_corner_radius_for_test();
    ASSERT_GT(start_cursor.right, start_cursor.left);
    ASSERT_GE(start_cursor.left, tight_active.left + tight_horizontal_safe_inset);
    ASSERT_LE(start_cursor.right, tight_active.right - tight_horizontal_safe_inset);
    ASSERT_GE(start_cursor.top, tight_active.top + tight_safe_inset);
    ASSERT_LE(start_cursor.bottom, tight_active.bottom - tight_safe_inset);

    window.set_preedit("ji'shu", 2, 0, 0, 2, true);
    window.update(page);
    const RECT end_active = window.preedit_active_rect_for_test();
    const RECT focused_end_cursor = window.preedit_cursor_rect_for_test();
    ASSERT_GT(focused_end_cursor.right, focused_end_cursor.left);
    ASSERT_GE(focused_end_cursor.left, end_active.left + tight_horizontal_safe_inset);
    ASSERT_LE(focused_end_cursor.right, end_active.right - tight_horizontal_safe_inset);
    ASSERT_GE(focused_end_cursor.top, end_active.top + tight_safe_inset);
    ASSERT_LE(focused_end_cursor.bottom, end_active.bottom - tight_safe_inset);

    config.layout_config.preedit_highlight_padding_x = 4;
    config.layout_config.preedit_highlight_padding_y = 2;
    config.layout_config.preedit_highlight_corner = 3;
    config.layout_config.preedit_highlight_border_width = 1;
    window.set_config(config);
    auto assert_cursor_before_run = [&](const char* preedit, std::size_t cursor,
                                        const char* suffix_text, bool syllable_boundaries) {
        window.set_preedit(preedit, cursor, 0, 0, std::strlen(preedit),
                           syllable_boundaries);
        window.update(page);
        const RECT slot_cursor = window.preedit_cursor_rect_for_test();
        ASSERT_GT(slot_cursor.right, slot_cursor.left);
        const auto& runs = window.preedit_runs_for_test();
        const auto suffix = std::find_if(runs.begin(), runs.end(), [&](const auto& run) {
            return run.text == suffix_text;
        });
        ASSERT_TRUE(suffix != runs.end());
        ASSERT_LE(slot_cursor.right + text_gap, suffix->rect.left);
    };
    assert_cursor_before_run("ni'hao", 3, "hao", true);
    assert_cursor_before_run("ni", 1, "i", false);
    assert_cursor_before_run("ni'hao", 2, "'", true);
    const std::string first_chinese_character = u8"你";
    assert_cursor_before_run(u8"你好", first_chinese_character.size(), u8"好", false);

    window.destroy();
}

TEST(CandidateWindow, dpi_relayout_notifies_controller) {
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);
    window.show();

    int callback_count = 0;
    window.set_layout_changed_callback([&callback_count]() { ++callback_count; });
    const UINT old_dpi = window.dpi();
    const UINT next_dpi = old_dpi == 96 ? 120 : 96;
    RECT suggested = {};
    ASSERT_TRUE(window.get_window_rect(&suggested));
    SendMessageW(window.hwnd_for_test(), WM_DPICHANGED, MAKELPARAM(next_dpi, next_dpi),
                 reinterpret_cast<LPARAM>(&suggested));

    ASSERT_EQ(callback_count, 1);
    ASSERT_TRUE(window.is_visible());
    ASSERT_TRUE(window.visible_candidate_count() > 0);
    window.destroy();
}

TEST(CandidateWindow, dpi_relayout_reapplies_caret_position) {
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.update(page);

    HWND hwnd = window.hwnd_for_test();
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    ASSERT_TRUE(GetMonitorInfoW(monitor, &monitor_info) != FALSE);
    RECT caret = {monitor_info.rcMonitor.left + 100, monitor_info.rcMonitor.top + 100,
                  monitor_info.rcMonitor.left + 101, monitor_info.rcMonitor.top + 120};
    window.move_to_caret(caret);
    window.show();

    RECT before = {};
    ASSERT_TRUE(window.get_window_rect(&before));
    const int width = before.right - before.left;
    const int height = before.bottom - before.top;
    RECT suggested = {monitor_info.rcMonitor.left + 8, monitor_info.rcMonitor.top + 8,
                      monitor_info.rcMonitor.left + 8 + width,
                      monitor_info.rcMonitor.top + 8 + height};
    const UINT old_dpi = window.dpi();
    const UINT next_dpi = old_dpi == 96 ? 120 : 96;
    SendMessageW(hwnd, WM_DPICHANGED, MAKELPARAM(next_dpi, next_dpi),
                 reinterpret_cast<LPARAM>(&suggested));

    RECT after = {};
    ASSERT_TRUE(window.get_window_rect(&after));
    const cxxime::CandidateWindowPlacement expected =
        cxxime::calculate_candidate_window_position(
            caret, after.right - after.left, after.bottom - after.top, 4,
            monitor_info.rcMonitor, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(after.left, expected.position.x);
    ASSERT_EQ(after.top, expected.position.y);

    window.destroy();
}

TEST(CandidateWindow, owner_can_follow_active_context_window) {
    HWND first_owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                       nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND context_window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, first_owner,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND second_owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(first_owner != nullptr);
    ASSERT_TRUE(context_window != nullptr);
    ASSERT_TRUE(second_owner != nullptr);

    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    HWND candidate = window.hwnd_for_test();
    ASSERT_TRUE(candidate != nullptr);

    window.set_owner(context_window);
    ASSERT_TRUE(window.owner_matches(context_window));
    candidate = window.hwnd_for_test();
    ASSERT_TRUE(candidate != nullptr);
    ASSERT_EQ(GetWindow(candidate, GW_OWNER), context_window);

    window.set_owner(second_owner);
    candidate = window.hwnd_for_test();
    ASSERT_TRUE(candidate != nullptr);
    ASSERT_EQ(GetWindow(candidate, GW_OWNER), second_owner);

    window.hide();
    ASSERT_TRUE(GetWindow(candidate, GW_OWNER) == nullptr);

    window.destroy();
    DestroyWindow(second_owner);
    DestroyWindow(first_owner);
}

TEST(CandidateWindow, ensure_created_recovers_destroyed_window_and_owner) {
    HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(owner != nullptr);

    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    HWND stale_window = window.hwnd_for_test();
    ASSERT_TRUE(stale_window != nullptr);
    ASSERT_TRUE(DestroyWindow(stale_window) != FALSE);
    ASSERT_TRUE(!window.is_created());

    ASSERT_TRUE(window.ensure_created(owner));
    ASSERT_TRUE(window.is_created());
    ASSERT_TRUE(window.owner_matches(owner));

    window.destroy();
    DestroyWindow(owner);
}

TEST(CandidateWindow, ensure_created_falls_back_when_preferred_owner_is_unavailable) {
    HWND stale_owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                       nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(stale_owner != nullptr);
    ASSERT_TRUE(DestroyWindow(stale_owner) != FALSE);
    ASSERT_TRUE(IsWindow(stale_owner) == FALSE);

    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    bool ownerless = false;
    ASSERT_TRUE(window.ensure_created_with_ownerless_fallback(stale_owner, &ownerless));
    ASSERT_TRUE(ownerless);
    ASSERT_TRUE(window.owner_matches(nullptr));

    window.destroy();
}

TEST(CandidateWindow, ensure_created_does_not_fall_back_from_unavailable_owner) {
    HWND current_owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(current_owner != nullptr);
    HWND stale_owner = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                       nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(stale_owner != nullptr);
    ASSERT_TRUE(DestroyWindow(stale_owner) != FALSE);

    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(current_owner, config));
    ASSERT_TRUE(!window.ensure_created(stale_owner));
    ASSERT_TRUE(window.owner_matches(current_owner));

    window.destroy();
    DestroyWindow(current_owner);
}

TEST(CandidateWindow, width_is_clamped_to_monitor_bounds) {
    cxxime::Config config;
    config.render_backend = "gdi";
    config.layout = "horizontal";
    config.layout_config.min_width = 100000;
    config.layout_config.max_width = 100000;

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text.assign(4096, 'w');
    page.candidates.push_back(std::move(candidate));
    page.extent.known_count = 2;
    page.extent.state = cxxime::CandidateExtentState::kHasMore;

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    HWND hwnd = window.hwnd_for_test();
    ASSERT_TRUE(hwnd != nullptr);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    ASSERT_TRUE(GetMonitorInfoW(monitor, &monitor_info) != FALSE);

    RECT caret_rect = {monitor_info.rcMonitor.left + 16, monitor_info.rcMonitor.top + 16,
                       monitor_info.rcMonitor.left + 16, monitor_info.rcMonitor.top + 36};
    window.move_to_caret(caret_rect);
    window.update(page);

    RECT window_rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &window_rect) != FALSE);
    ASSERT_LE(window_rect.right - window_rect.left,
              monitor_info.rcMonitor.right - monitor_info.rcMonitor.left);

    window.show();
    ASSERT_EQ(window.visible_candidate_count(), 1);
    window.hide();
    ASSERT_EQ(window.visible_candidate_count(), 0);
    window.destroy();
}

TEST(CandidateWindow, automatic_width_uses_comfortable_monitor_limit) {
    cxxime::Config config;
    config.render_backend = "gdi";
    config.layout = "horizontal";
    config.layout_config.max_width = 0;

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text.assign(4096, 'w');
    page.candidates.push_back(std::move(candidate));

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    HWND hwnd = window.hwnd_for_test();
    ASSERT_TRUE(hwnd != nullptr);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    ASSERT_TRUE(GetMonitorInfoW(monitor, &monitor_info) != FALSE);

    RECT caret_rect = {monitor_info.rcMonitor.left + 16, monitor_info.rcMonitor.top + 16,
                       monitor_info.rcMonitor.left + 16, monitor_info.rcMonitor.top + 36};
    window.move_to_caret(caret_rect);
    window.update(page);

    HDC dc = GetDC(hwnd);
    const float dpi_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
    ReleaseDC(hwnd, dc);
    const int display_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const int expected_max_width =
        cxxime::calculate_auto_candidate_window_max_width(display_width, dpi_scale);
    RECT window_rect = {};
    ASSERT_TRUE(GetWindowRect(hwnd, &window_rect) != FALSE);
    ASSERT_LE(window_rect.right - window_rect.left, expected_max_width);

    window.destroy();
}

TEST(CandidateWindow, placement_survives_hide_until_explicit_reset) {
    test::ScopedDpiAwarenessContext dpi_awareness;
    cxxime::Config config;
    config.render_backend = "gdi";
    config.layout = "horizontal";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::CandidateWindow window;
    ASSERT_TRUE(window.create(nullptr, config));
    window.set_preedit("ni'hao");
    window.update(page);

    HWND hwnd = window.hwnd_for_test();
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    ASSERT_TRUE(GetMonitorInfoW(monitor, &monitor_info) != FALSE);
    const SIZE window_size = window.window_size();
    ASSERT_GT(window_size.cx, 0);
    ASSERT_GT(window_size.cy, 0);
    const int monitor_height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    ASSERT_GT(monitor_height, window_size.cy * 2 + 8);
    const int window_width = window_size.cx;
    const int window_height = window_size.cy;
    RECT edge_caret = {monitor_info.rcMonitor.left + window_width,
                       monitor_info.rcMonitor.bottom - window_height / 2,
                       monitor_info.rcMonitor.left + window_width + 1,
                       monitor_info.rcMonitor.bottom - window_height / 2 + 1};
    const auto edge_expected = cxxime::calculate_candidate_window_position(
        edge_caret, window_width, window_height, 4, monitor_info.rcMonitor,
        cxxime::CandidatePlacementSide::Unset);
    ASSERT_EQ(edge_expected.side, cxxime::CandidatePlacementSide::Above);
    window.move_to_caret(edge_caret);
    window.show();

    RECT above_rect = {};
    ASSERT_TRUE(window.get_window_rect(&above_rect));
    ASSERT_EQ(above_rect.left, edge_expected.position.x);
    ASSERT_EQ(above_rect.top, edge_expected.position.y);

    window.hide();
    const int center_top = monitor_info.rcMonitor.top + (monitor_height - window_height) / 2;
    RECT center_caret = {monitor_info.rcMonitor.left + window_width, center_top,
                         monitor_info.rcMonitor.left + window_width + 1, center_top + 1};
    const auto preserved_expected = cxxime::calculate_candidate_window_position(
        center_caret, window_width, window_height, 4, monitor_info.rcMonitor,
        cxxime::CandidatePlacementSide::Above);
    ASSERT_EQ(preserved_expected.side, cxxime::CandidatePlacementSide::Above);
    window.move_to_caret(center_caret);
    window.show();
    RECT preserved_rect = {};
    ASSERT_TRUE(window.get_window_rect(&preserved_rect));
    ASSERT_EQ(preserved_rect.left, preserved_expected.position.x);
    ASSERT_EQ(preserved_rect.top, preserved_expected.position.y);

    window.reset_placement();
    window.move_to_caret(center_caret);
    RECT reset_rect = {};
    ASSERT_TRUE(window.get_window_rect(&reset_rect));
    const auto reset_expected = cxxime::calculate_candidate_window_position(
        center_caret, window_width, window_height, 4, monitor_info.rcMonitor,
        cxxime::CandidatePlacementSide::Unset);
    ASSERT_EQ(reset_expected.side, cxxime::CandidatePlacementSide::Below);
    ASSERT_EQ(reset_rect.left, reset_expected.position.x);
    ASSERT_EQ(reset_rect.top, reset_expected.position.y);

    window.destroy();
}

TEST(CandidateWindow, remains_above_visible_status_window_during_updates) {
    test::ScopedDpiAwarenessContext dpi_awareness;
    cxxime::Config config;
    config.render_backend = "gdi";

    cxxime::CandidatePage page;
    cxxime::Candidate candidate;
    candidate.text = "candidate";
    page.candidates.push_back(candidate);

    cxxime::StatusWindow status_window;
    ASSERT_TRUE(status_window.create(cxxime::StatusTheme{}));
    status_window.show();
    RECT status_rect = {};
    ASSERT_TRUE(status_window.get_window_rect(&status_rect));

    cxxime::CandidateWindow candidate_window;
    ASSERT_TRUE(candidate_window.create(nullptr, config));
    candidate_window.update(page);
    candidate_window.move_to_screen_position(status_rect.left, status_rect.top);
    candidate_window.show();
    RECT candidate_rect = {};
    ASSERT_TRUE(candidate_window.get_window_rect(&candidate_rect));
    RECT intersection = {};
    ASSERT_TRUE(IntersectRect(&intersection, &candidate_rect, &status_rect) != FALSE);
    ASSERT_TRUE(window_is_above(candidate_window.hwnd_for_test(),
                                status_window.hwnd_for_test()));

    status_window.show();
    ASSERT_TRUE(window_is_above(status_window.hwnd_for_test(),
                                candidate_window.hwnd_for_test()));

    status_window.show_below(candidate_window.native_handle());
    ASSERT_TRUE(window_is_above(candidate_window.hwnd_for_test(),
                                status_window.hwnd_for_test()));
    ASSERT_TRUE((GetWindowLongPtrW(status_window.hwnd_for_test(), GWL_EXSTYLE) &
                 WS_EX_TOPMOST) != 0);

    status_window.hide();
    status_window.show_below(candidate_window.native_handle());
    ASSERT_TRUE(window_is_above(candidate_window.hwnd_for_test(),
                                status_window.hwnd_for_test()));

    cxxime::ButtonState state;
    state.full_shape = true;
    status_window.update_state(state);
    ASSERT_TRUE(window_is_above(candidate_window.hwnd_for_test(),
                                status_window.hwnd_for_test()));

    candidate_window.destroy();
    status_window.destroy();
}

RUN_ALL_TESTS()
