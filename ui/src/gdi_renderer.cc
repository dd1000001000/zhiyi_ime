// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "gdi_renderer.h"

#include <algorithm>
#include <cmath>

#include <cxxime/config.h>

#include "sparkle.h"

namespace cxxime {

static std::wstring to_wstr(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}
static COLORREF clr(Color c) { return RGB(c.r, c.g, c.b); }

static Color separator_color(const Theme* theme) {
    if (!theme)
        return {160, 160, 160, 255};
    return {
        (uint8_t)((theme->background.r * 3 + theme->text.r) / 4),
        (uint8_t)((theme->background.g * 3 + theme->text.g) / 4),
        (uint8_t)((theme->background.b * 3 + theme->text.b) / 4),
        255,
    };
}

static void draw_preedit_separator(HDC dc, const RECT& clip, const RenderContext& ctx,
                                   int margin) {
    auto* cfg = ctx.layout_cfg;
    int sep_y = ctx.preedit_rect.bottom + (cfg ? cfg->spacing / 3 : 5);
    const Color color = ctx.high_contrast ? ctx.theme->border : separator_color(ctx.theme);
    HPEN sep = CreatePen(PS_SOLID, 1, clr(color));
    HPEN old_p = (HPEN)SelectObject(dc, sep);
    MoveToEx(dc, margin + 2, sep_y, nullptr);
    LineTo(dc, clip.right - margin - 2, sep_y);
    SelectObject(dc, old_p);
    DeleteObject(sep);
}

static void draw_border(HDC dc, const RECT& clip, const RenderContext& ctx) {
    auto* cfg = ctx.layout_cfg;
    if (!ctx.theme || !cfg || cfg->border_width <= 0)
        return;
    int bw = cfg->border_width;
    int inset = bw / 2;
    HPEN border_pen = CreatePen(PS_SOLID, bw, clr(ctx.theme->border));
    HPEN old_pen = (HPEN)SelectObject(dc, border_pen);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, inset, inset, clip.right - inset, clip.bottom - inset,
              cfg->round_corner_ex, cfg->round_corner_ex);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);
}

static void draw_preedit(HDC dc, const RenderContext& ctx, HFONT font, COLORREF text_color,
                         COLORREF cursor_color) {
    if (ctx.preedit.empty() || ctx.preedit_rect.right <= ctx.preedit_rect.left || !font ||
        !ctx.theme) {
        return;
    }

    HFONT old_font = static_cast<HFONT>(SelectObject(dc, font));
    if (ctx.preedit_active_rect.right > ctx.preedit_active_rect.left) {
        const COLORREF active_back = clr(ctx.theme->preedit_active_back);
        const COLORREF active_border = clr(ctx.theme->preedit_active_border);
        HBRUSH brush = CreateSolidBrush(active_back);
        HPEN pen = ctx.preedit_border_width > 0
                       ? CreatePen(PS_SOLID, ctx.preedit_border_width, active_border)
                       : static_cast<HPEN>(GetStockObject(NULL_PEN));
        HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(dc, brush));
        HPEN old_pen = static_cast<HPEN>(SelectObject(dc, pen));
        const int corner_diameter = ctx.preedit_corner_radius * 2;
        RoundRect(dc, ctx.preedit_active_rect.left, ctx.preedit_active_rect.top,
                  ctx.preedit_active_rect.right, ctx.preedit_active_rect.bottom,
                  corner_diameter, corner_diameter);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        if (ctx.preedit_border_width > 0) {
            DeleteObject(pen);
        }
        DeleteObject(brush);
    }

    SetBkMode(dc, TRANSPARENT);
    for (const auto& run : ctx.preedit_runs) {
        COLORREF color = ctx.high_contrast ? clr(ctx.theme->preedit_text) : text_color;
        if (ctx.high_contrast && run.focused) {
            color = clr(ctx.theme->hilited_text);
        } else if (!ctx.high_contrast && run.kind == PreeditRunKind::Converted) {
            color = clr(ctx.theme->text);
        } else if (!ctx.high_contrast && run.kind == PreeditRunKind::Separator) {
            color = clr(ctx.theme->preedit_separator);
        }
        SetTextColor(dc, color);
        const std::wstring text = to_wstr(run.text);
        DrawTextW(dc, text.c_str(), -1, const_cast<RECT*>(&run.rect),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (ctx.preedit_cursor_rect.right > ctx.preedit_cursor_rect.left) {
        if (ctx.preedit_cursor_in_focus) {
            if (ctx.high_contrast) {
                cursor_color = clr(ctx.theme->hilited_text);
            } else if (ctx.preedit_cursor_emphasized) {
                cursor_color = clr(ctx.theme->preedit_cursor);
            } else {
                cursor_color = clr(ctx.preedit_cursor_idle);
            }
        }
        HBRUSH cursor_brush = CreateSolidBrush(cursor_color);
        FillRect(dc, &ctx.preedit_cursor_rect, cursor_brush);
        DeleteObject(cursor_brush);
    }

    SelectObject(dc, old_font);
}

void GdiRenderer::initialize(HWND hwnd, const Theme& theme, UINT dpi) {
    hwnd_ = hwnd;
    bg_brush_      = CreateSolidBrush(clr(theme.background));
    hl_brush_      = CreateSolidBrush(clr(theme.hilited_back));
    text_color_    = clr(theme.text);
    comment_color_ = clr(theme.comment_text);
    hl_text_color_ = clr(theme.hilited_text);
    preedit_color_ = clr(theme.preedit_text);
    preedit_cursor_color_ = clr(theme.preedit_cursor);
    label_color_   = clr(theme.label_text);
    nav_color_     = clr(theme.prev_page);

    BYTE hr = (BYTE)((theme.hilited_back.r + theme.background.r) / 2);
    BYTE hg = (BYTE)((theme.hilited_back.g + theme.background.g) / 2);
    BYTE hb = (BYTE)((theme.hilited_back.b + theme.background.b) / 2);
    hover_brush_ = CreateSolidBrush(RGB(hr, hg, hb));

    hfont_ = CreateFontW(-MulDiv(theme.font_size, dpi, 72),
                         0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, theme.font_name.c_str());
    preedit_font_ = CreateFontW(-MulDiv(theme.preedit_font_size, dpi, 72),
                                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, theme.font_name.c_str());
    nav_font_ = CreateFontW(-MulDiv(9, dpi, 72),
                            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, theme.font_name.c_str());
}

void GdiRenderer::render(HDC hdc, const RECT& clip, const RenderContext& ctx) {
    HDC target_dc = hdc;
    HDC buffer_dc = nullptr;
    HBITMAP buffer_bitmap = nullptr;
    HBITMAP old_bitmap = nullptr;
    int width = clip.right - clip.left;
    int height = clip.bottom - clip.top;

    if (width > 0 && height > 0) {
        buffer_dc = CreateCompatibleDC(hdc);
        buffer_bitmap = CreateCompatibleBitmap(hdc, width, height);
        if (buffer_dc && buffer_bitmap) {
            old_bitmap = (HBITMAP)SelectObject(buffer_dc, buffer_bitmap);
            SetViewportOrgEx(buffer_dc, -clip.left, -clip.top, nullptr);
            target_dc = buffer_dc;
        } else {
            if (buffer_dc) {
                DeleteDC(buffer_dc);
                buffer_dc = nullptr;
            }
            if (buffer_bitmap) {
                DeleteObject(buffer_bitmap);
                buffer_bitmap = nullptr;
            }
        }
    }

    FillRect(target_dc, &clip, bg_brush_);
    auto* cfg = ctx.layout_cfg;
    int margin = cfg ? cfg->margin_x : 12;
    int corner = cfg ? cfg->round_corner : 4;

    if (!ctx.rects || ctx.rects->empty()) {
        // No candidates — show preedit if available, otherwise placeholder
        if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left && preedit_font_) {
            draw_preedit(target_dc, ctx, preedit_font_, preedit_color_,
                         preedit_cursor_color_);
        } else {
            SetBkMode(target_dc, TRANSPARENT);
            SetTextColor(target_dc, preedit_color_);
            DrawTextW(target_dc, L"知意输入法", -1, const_cast<RECT*>(&clip), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left)
            draw_preedit_separator(target_dc, clip, ctx, margin);
        draw_border(target_dc, clip, ctx);
        if (buffer_dc) {
            BitBlt(hdc, clip.left, clip.top, width, height, buffer_dc, 0, 0, SRCCOPY);
            SelectObject(buffer_dc, old_bitmap);
            DeleteObject(buffer_bitmap);
            DeleteDC(buffer_dc);
        }
        return;
    }

    HFONT old_font = (HFONT)SelectObject(target_dc, hfont_);
    SetBkMode(target_dc, TRANSPARENT);

    // Preedit (smaller font)
    if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left && preedit_font_) {
        draw_preedit(target_dc, ctx, preedit_font_, preedit_color_,
                     preedit_cursor_color_);
        // Thin separator line between preedit and candidates
        draw_preedit_separator(target_dc, clip, ctx, margin);
    }

    // Candidates
    for (const auto& cr : *ctx.rects) {
        int i = cr.index;
        bool hl = (i == ctx.highlighted);
        bool hv = ctx.hovered_target == CandidateHoverTarget::Candidate &&
                  i == ctx.hovered_candidate_index;

        const bool recommended = cr.recommended && cr.mark_rect.right > cr.mark_rect.left &&
                                 ctx.theme;
        if (hl && recommended) {
            // The gradient highlight of a recommended candidate.
            TRIVERTEX v[2] = {};
            v[0].x = cr.highlight_rect.left;
            v[0].y = cr.highlight_rect.top;
            v[0].Red = kRecommendHighlightBlue.r << 8;
            v[0].Green = kRecommendHighlightBlue.g << 8;
            v[0].Blue = kRecommendHighlightBlue.b << 8;
            v[1].x = cr.highlight_rect.right;
            v[1].y = cr.highlight_rect.bottom;
            v[1].Red = kRecommendHighlightPurple.r << 8;
            v[1].Green = kRecommendHighlightPurple.g << 8;
            v[1].Blue = kRecommendHighlightPurple.b << 8;
            GRADIENT_RECT gr = {0, 1};
            HRGN round = CreateRoundRectRgn(cr.highlight_rect.left, cr.highlight_rect.top,
                                            cr.highlight_rect.right + 1,
                                            cr.highlight_rect.bottom + 1, corner, corner);
            const int saved = SaveDC(target_dc);
            ExtSelectClipRgn(target_dc, round, RGN_AND);
            GradientFill(target_dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
            RestoreDC(target_dc, saved);
            DeleteObject(round);
        } else if (hl || hv) {
            HBRUSH use = hl ? hl_brush_ : hover_brush_;
            HBRUSH ob = (HBRUSH)SelectObject(target_dc, use);
            HPEN op = (HPEN)SelectObject(target_dc, GetStockObject(NULL_PEN));
            RoundRect(target_dc, cr.highlight_rect.left, cr.highlight_rect.top,
                cr.highlight_rect.right, cr.highlight_rect.bottom, corner, corner);
            SelectObject(target_dc, op);
            SelectObject(target_dc, ob);
        }

        // Label "N. "
        SetTextColor(target_dc, hl ? hl_text_color_ : label_color_);
        std::wstring label = candidate_label(i);
        DrawTextW(target_dc, label.c_str(), -1, const_cast<RECT*>(&cr.label_rect),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Candidate text
        SetTextColor(target_dc, hl ? hl_text_color_ : text_color_);
        DrawTextW(target_dc, to_wstr(cr.text).c_str(), -1, const_cast<RECT*>(&cr.text_rect),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (recommended) {
            // GDI fallback: a solid star (white on the gradient highlight, purple elsewhere).
            const SparkleStar star = sparkle_star(cr.mark_rect, ctx.sparkle_t);
            if (star.r >= 0.5f) {
                const Color c = hl ? Color{255, 255, 255, 255} : kRecommendPurple;
                HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
                HBRUSH ob = (HBRUSH)SelectObject(target_dc, brush);
                HPEN op = (HPEN)SelectObject(target_dc, GetStockObject(NULL_PEN));
                const auto pts = sparkle_polygon(star);
                Polygon(target_dc, pts.data(), static_cast<int>(pts.size()));
                SelectObject(target_dc, op);
                SelectObject(target_dc, ob);
                DeleteObject(brush);
            }
        }
        if (!cr.comment.empty()) {
            SetTextColor(target_dc, hl ? hl_text_color_ : (hv ? text_color_ : comment_color_));
            DrawTextW(target_dc, to_wstr(cr.comment).c_str(), -1,
                    const_cast<RECT*>(&cr.comment_rect),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // Page nav (always visible, dimmed when disabled)
    if (ctx.page_total > 1 && nav_font_) {
        HFONT old_nav = (HFONT)SelectObject(target_dc, nav_font_);
        int nc = corner > 2 ? corner - 1 : 1;
        bool pe = (ctx.page_current > 1), ne = (ctx.page_current < ctx.page_total);
        // Dim color for disabled state: close to background
        COLORREF dim = clr({(uint8_t)((ctx.theme->background.r * 3 + ctx.theme->text.r) / 4),
            (uint8_t)((ctx.theme->background.g * 3 + ctx.theme->text.g) / 4),
            (uint8_t)((ctx.theme->background.b * 3 + ctx.theme->text.b) / 4), 255});
        // <
        {
            bool h = pe && ctx.hovered_target == CandidateHoverTarget::PreviousPage;
            if (h) {
                HBRUSH ob = (HBRUSH)SelectObject(target_dc, hl_brush_);
                HPEN op = (HPEN)SelectObject(target_dc, GetStockObject(NULL_PEN));
                RoundRect(target_dc, ctx.prev_button_rect.left, ctx.prev_button_rect.top,
                    ctx.prev_button_rect.right, ctx.prev_button_rect.bottom, nc, nc);
                SelectObject(target_dc, op);
                SelectObject(target_dc, ob);
            }
            SetTextColor(target_dc, h ? hl_text_color_ : (pe ? nav_color_ : dim));
            DrawTextW(target_dc, L"<", 1, const_cast<RECT*>(&ctx.prev_button_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // >
        {
            bool h = ne && ctx.hovered_target == CandidateHoverTarget::NextPage;
            if (h) {
                HBRUSH ob = (HBRUSH)SelectObject(target_dc, hl_brush_);
                HPEN op = (HPEN)SelectObject(target_dc, GetStockObject(NULL_PEN));
                RoundRect(target_dc, ctx.next_button_rect.left, ctx.next_button_rect.top,
                    ctx.next_button_rect.right, ctx.next_button_rect.bottom, nc, nc);
                SelectObject(target_dc, op);
                SelectObject(target_dc, ob);
            }
            SetTextColor(target_dc, h ? hl_text_color_ : (ne ? nav_color_ : dim));
            DrawTextW(target_dc, L">", 1, const_cast<RECT*>(&ctx.next_button_rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(target_dc, old_nav);
    }

    draw_border(target_dc, clip, ctx);

    SelectObject(target_dc, old_font);

    if (buffer_dc) {
        BitBlt(hdc, clip.left, clip.top, width, height, buffer_dc, 0, 0, SRCCOPY);
        SelectObject(buffer_dc, old_bitmap);
        DeleteObject(buffer_bitmap);
        DeleteDC(buffer_dc);
    }
}

void GdiRenderer::finalize() {
    if (hfont_)        { DeleteObject(hfont_); hfont_ = nullptr; }
    if (preedit_font_) { DeleteObject(preedit_font_); preedit_font_ = nullptr; }
    if (nav_font_)     { DeleteObject(nav_font_); nav_font_ = nullptr; }
    if (bg_brush_)     { DeleteObject(bg_brush_); bg_brush_ = nullptr; }
    if (hl_brush_)     { DeleteObject(hl_brush_); hl_brush_ = nullptr; }
    if (hover_brush_)  { DeleteObject(hover_brush_); hover_brush_ = nullptr; }
}

} // namespace cxxime
