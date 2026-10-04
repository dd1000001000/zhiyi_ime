// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_RENDER_CONTEXT_H_
#define CXXIME_RENDER_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include <cxxime/candidate.h>

namespace cxxime {

struct Color { uint8_t r, g, b, a; };

struct Theme {
    Color background{255, 255, 255, 255};
    Color text{0, 0, 0, 255};               // normal candidate text
    Color comment_text{102, 102, 102, 255}; // normal candidate comment text
    Color label_text{128, 128, 128, 255};   // label "1. " text
    Color preedit_text{128, 128, 128, 255}; // preedit text
    Color preedit_separator{102, 102, 102, 255};
    Color preedit_active_back{232, 240, 248, 255};
    Color preedit_active_border{176, 200, 224, 255};
    Color preedit_cursor{0, 120, 215, 255}; // static cursor in popup preedit
    Color hilited_text{255, 255, 255, 255}; // highlighted candidate text
    Color hilited_back{0, 120, 215, 255};   // highlighted candidate background
    Color border{200, 200, 200, 255};       // window/separator border
    Color prev_page{128, 128, 128, 255};    // page nav arrow color
    Color next_page{128, 128, 128, 255};
    int font_size = 14;
    int preedit_font_size = 12;
    std::wstring font_name = L"Microsoft YaHei UI";
};

struct StatusTheme {
    Color back{243, 243, 243, 255};
    Color border{200, 200, 200, 255};
    Color inactive_back{232, 232, 232, 200};
    Color inactive_text{72, 72, 72, 255};
    Color pinyin_mode_text{42, 102, 173, 255};
    Color wubi_mode_text{179, 68, 63, 255};
    Color mixed_mode_text{15, 118, 110, 255};
    Color separator{212, 212, 212, 255};
};

struct CandidateRect {
    int index;
    std::string text;
    std::string comment;
    RECT label_rect{};
    RECT text_rect{};
    RECT comment_rect{};
    RECT highlight_rect{};
    bool recommended = false;  // Laya's pick: sparkle at the top-right corner
    RECT mark_rect{};          // space reserved after the text for the sparkle
};

// Width reserved after a recommended candidate for its sparkle mark.
inline int recommendation_mark_width(int row_height) { return row_height / 2; }

Theme make_light_theme();
Theme make_dark_theme();
Theme get_theme(const std::string& scheme_name);

struct Config;
Theme build_theme_from_config(const Config& cfg);

struct LayoutConfig;
enum class CandidateHoverTarget { None, Candidate, PreviousPage, NextPage };
enum class PreeditRunKind { Converted, Active, Separator };

struct PreeditTextRun {
    std::string text;
    RECT rect{};
    PreeditRunKind kind = PreeditRunKind::Active;
    bool focused = false;
};

struct RenderContext {
    const std::vector<CandidateRect>* rects = nullptr;
    const Theme* theme = nullptr;
    const LayoutConfig* layout_cfg = nullptr;
    std::string preedit;
    size_t preedit_cursor = 0;
    bool preedit_cursor_in_focus = false;
    bool preedit_cursor_emphasized = false;
    Color preedit_cursor_idle{};
    int page_current = 1, page_total = 1;
    int highlighted = -1;
    // Seconds since the Laya recommendation mark appeared while it twinkles; < 0 = steady.
    float sparkle_t = -1.0f;
    CandidateHoverTarget hovered_target = CandidateHoverTarget::None;
    int hovered_candidate_index = -1;
    RECT preedit_rect{};
    RECT preedit_active_rect{};
    RECT preedit_cursor_rect{};
    int preedit_corner_radius = 3;
    int preedit_border_width = 1;
    bool high_contrast = false;
    std::vector<PreeditTextRun> preedit_runs;
    RECT page_indicator_rect{};
    RECT prev_button_rect{};
    RECT next_button_rect{};
    int preedit_text_height = 0;
};

} // namespace cxxime
#endif
