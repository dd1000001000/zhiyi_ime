// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Layout calculation modeled after Weasel's HorizontalLayout / VerticalLayout.

#include <cxxime/layout.h>

#include <algorithm>
#include <utility>

#include <cxxime/config.h>

namespace cxxime {

PageNavigationMetrics candidate_page_navigation_metrics(UINT dpi) {
    const UINT effective_dpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    return {(std::max)(1, MulDiv(16, effective_dpi, USER_DEFAULT_SCREEN_DPI)),
            (std::max)(1, MulDiv(2, effective_dpi, USER_DEFAULT_SCREEN_DPI)),
            (std::max)(1, MulDiv(4, effective_dpi, USER_DEFAULT_SCREEN_DPI))};
}

int calculate_auto_candidate_window_max_width(int work_area_width, float dpi_scale) {
    if (work_area_width <= 0) {
        return 0;
    }

    constexpr int kComfortableWidthDip = 960;
    constexpr int kWorkAreaPercent = 80;
    const float scale = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    const int comfortable_width = static_cast<int>(kComfortableWidthDip * scale + 0.5f);
    const int proportional_width = MulDiv(work_area_width, kWorkAreaPercent, 100);
    return (std::max)(1, (std::min)(comfortable_width, proportional_width));
}

static std::wstring to_wstr(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

class LayoutFont {
public:
    LayoutFont(const std::string& font_name, int font_size, UINT dpi) {
        const std::wstring wfont = to_wstr(font_name);
        handle_ = CreateFontW(
            -MulDiv(font_size, dpi, 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            wfont.c_str());
    }

    LayoutFont(const LayoutFont&) = delete;
    LayoutFont& operator=(const LayoutFont&) = delete;
    LayoutFont(LayoutFont&&) = delete;
    LayoutFont& operator=(LayoutFont&&) = delete;

    ~LayoutFont() {
        if (handle_) {
            DeleteObject(handle_);
        }
    }

    HFONT get() const { return handle_; }

private:
    HFONT handle_ = nullptr;
};

static int get_font_height(HDC hdc, HFONT hf) {
    TEXTMETRICW tm = {};
    if (hf) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, hf));
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
    }
    return tm.tmHeight;
}

static SIZE measure_wstr(HDC hdc, HFONT hf, const std::wstring& text) {
    SIZE sz = {};
    if (hf) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, hf));
        GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
        SelectObject(hdc, old);
    }
    return sz;
}

static int text_render_slack(int row_height) {
    return std::max(2, row_height / 4);
}

static std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &utf8[0], length,
                        nullptr, nullptr);
    return utf8;
}

static std::string format_comment(const Candidate& candidate) {
    if (candidate.comment.empty()) {
        return {};
    }
    return " (" + candidate.comment + ")";
}

static int candidate_content_right(const CandidateRect& rect) {
    return rect.comment.empty() ? rect.text_rect.right : rect.comment_rect.right;
}

static std::string truncate_middle(HDC hdc, const std::string& text, HFONT hf,
                                   int available_width) {
    std::wstring wide_text = to_wstr(text);
    if (measure_wstr(hdc, hf, wide_text).cx <= available_width) {
        return text;
    }

    const std::wstring ellipsis = L"…";  // \x2026
    int ellipsis_width =
        static_cast<int>(measure_wstr(hdc, hf, ellipsis).cx);
    int target_width = (std::max)(0, available_width - ellipsis_width);
    int prefix_target = target_width * 7 / 10;
    int suffix_target = target_width - prefix_target;

    int lo = 0;
    int hi = static_cast<int>(wide_text.size());
    int best_prefix = 0;
    while (lo <= hi) {
        int middle = (lo + hi) / 2;
        if (measure_wstr(hdc, hf, wide_text.substr(0, middle)).cx <= prefix_target) {
            best_prefix = middle;
            lo = middle + 1;
        } else {
            hi = middle - 1;
        }
    }

    int text_length = static_cast<int>(wide_text.size());
    lo = 0;
    hi = text_length;
    int best_suffix = 0;
    while (lo <= hi) {
        int middle = (lo + hi) / 2;
        if (measure_wstr(hdc, hf, wide_text.substr(text_length - middle, middle)).cx <=
            suffix_target) {
            best_suffix = middle;
            lo = middle + 1;
        } else {
            hi = middle - 1;
        }
    }

    if (best_prefix + best_suffix >= text_length) {
        return text;
    }
    return to_utf8(wide_text.substr(0, best_prefix) + ellipsis +
                   wide_text.substr(text_length - best_suffix));
}

// ===== Horizontal layout (Weasel-style) =====

LayoutResult calculate_horizontal_layout(HDC hdc,
    const std::vector<Candidate>& candidates,
    const std::string& font_name, int font_size,
    const LayoutConfig& cfg, int page_total, UINT dpi) {
    LayoutResult result;
    LayoutFont font(font_name, font_size, dpi);
    const HFONT hf = font.get();
    if (candidates.empty()) {
        result.width = cfg.min_width;
        result.row_height = get_font_height(hdc, hf);
        result.height = result.row_height + cfg.margin_y * 2;
        return result;
    }

    int rh = get_font_height(hdc, hf);
    result.row_height = rh;
    int text_slack = text_render_slack(rh);

    int max_w = cfg.max_width > 0 ? cfg.max_width : 600;
    int nav_extra = 0;
    if (page_total > 1) {
        const PageNavigationMetrics nav = candidate_page_navigation_metrics(dpi);
        const int nav_buttons_width = nav.button_width * 2 + nav.gap;
        int nav_overlap = cfg.hilite_padding_x + nav.leading_gap - cfg.candidate_spacing;
        nav_extra = nav_buttons_width + (std::max)(0, nav_overlap);
        max_w = (std::max)(1, max_w - nav_extra);
    }
    int min_w = (std::max)(0, cfg.min_width - nav_extra);
    if (min_w > max_w) {
        min_w = max_w;
    }

    int x = cfg.margin_x, y = cfg.margin_y;

    for (int i = 0; i < (int)candidates.size(); ++i) {
        std::wstring label = candidate_label(i);
        SIZE lsz = measure_wstr(hdc, hf, label);
        std::string comment = format_comment(candidates[i]);
        SIZE text_size = measure_wstr(hdc, hf, to_wstr(candidates[i].text));
        SIZE comment_size = measure_wstr(hdc, hf, to_wstr(comment));

        const int mark_w = candidates[i].recommended ? recommendation_mark_width(rh) : 0;
        int label_w = lsz.cx, text_w = text_size.cx + comment_size.cx + text_slack + mark_w;
        int total_w = label_w + cfg.hilite_spacing + text_w;

        // Non-first candidate doesn't fit → stop (first candidate always added)
        if (x > cfg.margin_x && x + total_w + cfg.candidate_spacing > max_w + cfg.margin_x)
            break;

        CandidateRect cr;
        cr.index = i;
        cr.text = candidates[i].text;
        cr.comment = std::move(comment);
        cr.recommended = candidates[i].recommended;
        cr.label_rect = {x, y, x + label_w, y + rh};
        int text_left = x + label_w + cfg.hilite_spacing;
        cr.text_rect = {text_left, y, text_left + text_size.cx + text_slack, y + rh};
        if (!cr.comment.empty()) {
            cr.comment_rect = {text_left + text_size.cx, y,
                            text_left + text_size.cx + comment_size.cx + text_slack,
                            y + rh};
        }

        const int content_right = candidate_content_right(cr);
        cr.mark_rect = {content_right, y, content_right + mark_w, y + rh};
        RECT bounds = {cr.label_rect.left, y, content_right + mark_w, y + rh};
        cr.highlight_rect = bounds;
        InflateRect(&cr.highlight_rect, cfg.hilite_padding_x, cfg.hilite_padding_y);

        result.rects.push_back(cr);
        x += total_w + cfg.candidate_spacing;
    }

    // Middle truncation: single candidate that exceeds available width
    if (result.rects.size() == 1) {
        auto& cr = result.rects[0];
        int text_avail = max_w - cfg.margin_x * 2
                         - (cr.text_rect.left - cr.label_rect.left) - cfg.candidate_spacing;
        int comment_width = cr.comment.empty()
                                ? 0
                                : measure_wstr(hdc, hf, to_wstr(cr.comment)).cx;
        int main_text_width = (std::max)(0, text_avail - comment_width - text_slack);
        std::string truncated = truncate_middle(hdc, cr.text, hf, main_text_width);
        if (truncated != cr.text) {
            cr.text = std::move(truncated);
            int width = measure_wstr(hdc, hf, to_wstr(cr.text)).cx;
            cr.text_rect.right = cr.text_rect.left + width + text_slack;
            if (!cr.comment.empty()) {
                cr.comment_rect.left = cr.text_rect.left + width;
                cr.comment_rect.right = cr.comment_rect.left + comment_width + text_slack;
            }
            const int mark_w = cr.mark_rect.right - cr.mark_rect.left;
            cr.mark_rect.left = candidate_content_right(cr);
            cr.mark_rect.right = cr.mark_rect.left + mark_w;
            cr.highlight_rect.right = cr.mark_rect.right + cfg.hilite_padding_x;
        }
    }

    // Final width: last candidate's right edge + margin, capped to max_w
    int content_w = cfg.margin_x;
    if (!result.rects.empty())
        content_w = (std::max)(candidate_content_right(result.rects.back()),
                               static_cast<int>(result.rects.back().mark_rect.right)) +
                    cfg.candidate_spacing;
    result.width = content_w + cfg.margin_x;
    if (result.width > max_w) result.width = max_w;
    if (result.width < min_w) result.width = min_w;
    // Add back nav space for window sizing
    result.width += nav_extra;
    result.height = rh + cfg.margin_y * 2;
    return result;
}

// ===== Vertical layout (Weasel-style) =====

LayoutResult calculate_vertical_layout(HDC hdc,
    const std::vector<Candidate>& candidates,
    const std::string& font_name, int font_size,
    const LayoutConfig& cfg, UINT dpi) {
    LayoutResult result;
    LayoutFont font(font_name, font_size, dpi);
    const HFONT hf = font.get();
    if (candidates.empty()) {
        result.width = cfg.min_width;
        result.row_height = get_font_height(hdc, hf);
        result.height = result.row_height + cfg.margin_y * 2;
        return result;
    }

    int rh = get_font_height(hdc, hf);
    result.row_height = rh;
    int text_slack = text_render_slack(rh);

    int max_w = cfg.max_width > 0 ? cfg.max_width : 600;
    int max_h = cfg.max_height > 0 ? cfg.max_height : 0;  // 0 = no limit

    // First pass: measure all to find widest label/text columns
    int widest_label = 0, widest_text = 0;
    for (int i = 0; i < (int)candidates.size(); ++i) {
        std::wstring label = candidate_label(i);
        int lw = measure_wstr(hdc, hf, label).cx;
        std::string comment = format_comment(candidates[i]);
        int tw = measure_wstr(hdc, hf, to_wstr(candidates[i].text)).cx +
                 measure_wstr(hdc, hf, to_wstr(comment)).cx + text_slack +
                 (candidates[i].recommended ? recommendation_mark_width(rh) : 0);
        if (lw > widest_label) widest_label = lw;
        if (tw > widest_text) widest_text = tw;
    }

    int text_x = cfg.margin_x + widest_label + cfg.hilite_spacing;

    // Clamp text column width to available space
    int avail_text_w = max_w - text_x - cfg.hilite_padding_x - cfg.margin_x;
    if (widest_text > avail_text_w) widest_text = avail_text_w;

    // Second pass: add candidates, stop when height exceeded
    int y = cfg.margin_y;
    for (int i = 0; i < (int)candidates.size(); ++i) {
        // Height limit: stop if this row wouldn't fit
        if (max_h > 0 && y + rh + cfg.margin_y > max_h)
            break;

        CandidateRect cr;
        cr.index = i;
        cr.text = candidates[i].text;
        cr.comment = format_comment(candidates[i]);
        cr.recommended = candidates[i].recommended;
        cr.label_rect = {cfg.margin_x, y, cfg.margin_x + widest_label, y + rh};
        int text_width = measure_wstr(hdc, hf, to_wstr(cr.text)).cx;
        int text_right = cr.comment.empty() ? text_x + widest_text
                                            : text_x + text_width + text_slack;
        cr.text_rect = {text_x, y, text_right, y + rh};
        if (!cr.comment.empty()) {
            int comment_width =
                measure_wstr(hdc, hf, to_wstr(cr.comment)).cx;
            cr.comment_rect = {text_x + text_width, y,
                               text_x + text_width + comment_width + text_slack, y + rh};
        }

        if (cr.recommended) {
            const int right = cr.comment.empty() ? text_x + text_width + text_slack
                                                 : static_cast<int>(cr.comment_rect.right);
            cr.mark_rect = {right, y, right + recommendation_mark_width(rh), y + rh};
        }
        RECT bounds = {cr.label_rect.left, y, text_x + widest_text, y + rh};
        cr.highlight_rect = bounds;
        InflateRect(&cr.highlight_rect, cfg.hilite_padding_x, cfg.hilite_padding_y);

        result.rects.push_back(cr);
        y += rh + cfg.candidate_spacing;
    }

    // Middle truncation: single candidate that exceeds available width
    if (result.rects.size() == 1) {
        auto& cr = result.rects[0];
        int comment_width = cr.comment.empty()
                                ? 0
                                : measure_wstr(hdc, hf, to_wstr(cr.comment)).cx;
        int main_text_width = (std::max)(0, widest_text - comment_width - text_slack);
        std::string truncated = truncate_middle(hdc, cr.text, hf, main_text_width);
        if (truncated != cr.text) {
            cr.text = std::move(truncated);
            int width = measure_wstr(hdc, hf, to_wstr(cr.text)).cx;
            cr.text_rect.right = cr.text_rect.left + width + text_slack;
            if (!cr.comment.empty()) {
                cr.comment_rect.left = cr.text_rect.left + width;
                cr.comment_rect.right = cr.comment_rect.left + comment_width + text_slack;
            }
        }
    }

    result.width = text_x + widest_text + cfg.hilite_padding_x + cfg.margin_x;
    if (result.width < cfg.min_width) result.width = cfg.min_width;
    if (cfg.max_width > 0 && result.width > cfg.max_width) result.width = cfg.max_width;
    if (!result.rects.empty())
        y -= cfg.candidate_spacing;
    result.height = y + cfg.margin_y;
    return result;
}

} // namespace cxxime
