// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_LAYOUT_H_
#define CXXIME_LAYOUT_H_

#include <string>
#include <vector>

#include <windows.h>

#include <cxxime/render_context.h>

namespace cxxime {

struct Candidate;
struct LayoutConfig;

struct LayoutResult {
    std::vector<CandidateRect> rects;
    int width = 0;
    int height = 0;
    int row_height = 0;  // measured font height in pixels
};

struct PageNavigationMetrics {
    int button_width = 0;
    int gap = 0;
    int leading_gap = 0;
};

PageNavigationMetrics candidate_page_navigation_metrics(UINT dpi);

// Returns the automatic candidate-window width limit in physical pixels.
int calculate_auto_candidate_window_max_width(int work_area_width, float dpi_scale);

// Weasel-style: measure label + text separately, compute sub-rects, highlight = Inflate(text_bounds, hilite_padding)
// page_total: when > 1, reserve space for page nav buttons in horizontal layout
LayoutResult calculate_horizontal_layout(HDC hdc,
    const std::vector<Candidate>& candidates,
    const std::string& font_name, int font_size,
    const LayoutConfig& cfg, int page_total = 1, UINT dpi = USER_DEFAULT_SCREEN_DPI,
    bool letter_labels = false);

LayoutResult calculate_vertical_layout(HDC hdc,
    const std::vector<Candidate>& candidates,
    const std::string& font_name, int font_size,
    const LayoutConfig& cfg, UINT dpi = USER_DEFAULT_SCREEN_DPI, bool letter_labels = false);

} // namespace cxxime
#endif
