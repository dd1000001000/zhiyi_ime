// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/renderer.h>

#include <algorithm>
#include <string>

#include <cxxime/config.h>

#include "sparkle.h"

namespace cxxime {

static D2D1_COLOR_F c2d(const Color& c) { return D2D1::ColorF(c.r/255.0f, c.g/255.0f, c.b/255.0f, c.a/255.0f); }

static D2D1_COLOR_F separator_color(const Theme* theme) {
    if (!theme)
        return D2D1::ColorF(160.0f / 255.0f, 160.0f / 255.0f, 160.0f / 255.0f, 1.0f);
    return D2D1::ColorF(
        (float)(theme->background.r * 3 + theme->text.r) / 4.0f / 255.0f,
        (float)(theme->background.g * 3 + theme->text.g) / 4.0f / 255.0f,
        (float)(theme->background.b * 3 + theme->text.b) / 4.0f / 255.0f,
        1.0f);
}

static std::wstring dec(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static IDWriteTextFormat* mkfmt(IDWriteFactory* f, const wchar_t* name, float sz, DWRITE_TEXT_ALIGNMENT ha) {
    IDWriteTextFormat* fmt = nullptr;
    f->CreateTextFormat(name, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        sz, L"zh-cn", &fmt);
    if (fmt) {
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trimming, nullptr);
    }
    return fmt;
}

bool D2DRenderer::initialize(HWND hwnd, const Theme& theme, UINT dpi) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    hr = d2d_factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
        &render_target_);
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    // CandidateWindow computes layout and HWND size in physical pixels.
    // Keep D2D coordinates in the same pixel space; otherwise high-DPI render
    // targets interpret our rectangles as DIP and the content gets clipped.
    render_target_->SetDpi(USER_DEFAULT_SCREEN_DPI, USER_DEFAULT_SCREEN_DPI);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &text_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &comment_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &bg_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &highlight_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &highlight_text_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.68f, 0.85f, 1.0f, 0.5f), &hover_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &preedit_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray),
                                          &preedit_separator_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                          &preedit_active_back_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray),
                                          &preedit_active_border_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue),
                                          &preedit_cursor_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &label_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &nav_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &border_brush_);
    if (!text_brush_ || !comment_brush_ || !bg_brush_ || !highlight_brush_ ||
        !highlight_text_brush_ || !hover_brush_ || !preedit_brush_ ||
        !preedit_separator_brush_ || !preedit_active_back_brush_ ||
        !preedit_active_border_brush_ || !preedit_cursor_brush_ || !label_brush_ ||
        !nav_brush_ || !border_brush_) {
        finalize();
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    float fsize = (float)theme.font_size * dpi / 72.0f;
    float psize = (float)theme.preedit_font_size * dpi / 72.0f;
    fmt_left_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), fsize,
                        DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_right_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), fsize,
                        DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_preedit_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), psize,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_small_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), 9.0f * dpi / 72.0f,
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!fmt_left_ || !fmt_right_ || !fmt_preedit_ || !fmt_small_) {
        finalize();
        return false;
    }
    return true;
}

void D2DRenderer::finalize() {
    for (auto* p : {&fmt_small_, &fmt_preedit_, &fmt_right_, &fmt_left_}) { if (*p) { (*p)->Release(); *p = nullptr; } }
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    for (auto* p : {&border_brush_, &nav_brush_, &label_brush_, &preedit_cursor_brush_,
                    &preedit_active_border_brush_, &preedit_active_back_brush_,
                    &preedit_separator_brush_, &preedit_brush_, &hover_brush_,
                    &highlight_text_brush_, &highlight_brush_, &bg_brush_, &comment_brush_,
                    &text_brush_})
    { if (*p) { (*p)->Release(); *p = nullptr; } }
    if (render_target_) { render_target_->Release(); render_target_ = nullptr; }
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }
}

void D2DRenderer::draw_preedit(const RenderContext& ctx) {
    if (ctx.preedit.empty() || ctx.preedit_rect.right <= ctx.preedit_rect.left ||
        !preedit_brush_ || !ctx.theme) {
        return;
    }

    if (ctx.preedit_active_rect.right > ctx.preedit_active_rect.left &&
        preedit_active_back_brush_ && preedit_active_border_brush_) {
        const D2D1_ROUNDED_RECT active = {
            D2D1::RectF(static_cast<float>(ctx.preedit_active_rect.left),
                        static_cast<float>(ctx.preedit_active_rect.top),
                        static_cast<float>(ctx.preedit_active_rect.right),
                        static_cast<float>(ctx.preedit_active_rect.bottom)),
            static_cast<float>(ctx.preedit_corner_radius),
            static_cast<float>(ctx.preedit_corner_radius),
        };
        render_target_->FillRoundedRectangle(active, preedit_active_back_brush_);
        if (ctx.preedit_border_width > 0) {
            render_target_->DrawRoundedRectangle(
                active, preedit_active_border_brush_,
                static_cast<float>(ctx.preedit_border_width));
        }
    }

    for (const auto& run : ctx.preedit_runs) {
        const std::wstring text = dec(run.text);
        ID2D1SolidColorBrush* brush = preedit_brush_;
        if (run.kind == PreeditRunKind::Converted) {
            brush = text_brush_;
        } else if (run.kind == PreeditRunKind::Separator) {
            brush = preedit_separator_brush_;
        }
        if (ctx.high_contrast && run.focused) {
            brush = highlight_text_brush_;
        }
        const D2D1_RECT_F rect = {
            static_cast<float>(run.rect.left), static_cast<float>(run.rect.top),
            static_cast<float>(run.rect.right), static_cast<float>(run.rect.bottom),
        };
        render_target_->DrawText(text.c_str(), static_cast<UINT32>(text.length()), fmt_preedit_,
                                 rect, brush);
    }

    if (preedit_cursor_brush_ &&
        ctx.preedit_cursor_rect.right > ctx.preedit_cursor_rect.left) {
        render_target_->FillRectangle(
            D2D1::RectF(static_cast<float>(ctx.preedit_cursor_rect.left),
                        static_cast<float>(ctx.preedit_cursor_rect.top),
                        static_cast<float>(ctx.preedit_cursor_rect.right),
                        static_cast<float>(ctx.preedit_cursor_rect.bottom)),
            preedit_cursor_brush_);
    }
}

void D2DRenderer::render(const RenderContext& ctx) {
    if (!render_target_) return;
    render_target_->BeginDraw();
    auto* cfg = ctx.layout_cfg;
    float corner = cfg ? (float)cfg->round_corner : 4.0f;
    float margin = cfg ? (float)cfg->margin_x : 12.0f;

    if (ctx.theme) {
        bg_brush_->SetColor(c2d(ctx.theme->background));
        text_brush_->SetColor(c2d(ctx.theme->text));
        comment_brush_->SetColor(c2d(ctx.theme->comment_text));
        highlight_brush_->SetColor(c2d(ctx.theme->hilited_back));
        highlight_text_brush_->SetColor(c2d(ctx.theme->hilited_text));
        preedit_brush_->SetColor(c2d(ctx.theme->preedit_text));
        preedit_separator_brush_->SetColor(c2d(ctx.theme->preedit_separator));
        preedit_active_back_brush_->SetColor(c2d(ctx.theme->preedit_active_back));
        preedit_active_border_brush_->SetColor(c2d(ctx.theme->preedit_active_border));
        Color cursor_color = ctx.theme->preedit_cursor;
        if (ctx.preedit_cursor_in_focus) {
            if (ctx.high_contrast) {
                cursor_color = ctx.theme->hilited_text;
            } else if (!ctx.preedit_cursor_emphasized) {
                cursor_color = ctx.preedit_cursor_idle;
            }
        }
        preedit_cursor_brush_->SetColor(c2d(cursor_color));
        label_brush_->SetColor(c2d(ctx.theme->label_text));
        nav_brush_->SetColor(c2d(ctx.theme->prev_page));
        border_brush_->SetColor(c2d(ctx.theme->border));
        D2D1::ColorF hover_col((ctx.theme->background.r + ctx.theme->hilited_back.r) / 2.0f / 255.0f,
                                (ctx.theme->background.g + ctx.theme->hilited_back.g) / 2.0f / 255.0f,
                                (ctx.theme->background.b + ctx.theme->hilited_back.b) / 2.0f / 255.0f, 1.0f);
        hover_brush_->SetColor(hover_col);
    }

    D2D1_SIZE_F sz = render_target_->GetSize();
    render_target_->FillRectangle(D2D1::RectF(0,0,sz.width,sz.height), bg_brush_);

    auto draw_preedit_separator = [&]() {
        if (ctx.preedit.empty() || ctx.preedit_rect.right <= ctx.preedit_rect.left ||
            !preedit_brush_)
            return;
        float sep_y = (float)ctx.preedit_rect.bottom + (cfg ? (float)cfg->spacing/3 : 5.0f);
        preedit_brush_->SetColor(ctx.high_contrast ? c2d(ctx.theme->border)
                                                   : separator_color(ctx.theme));
        render_target_->DrawLine({margin + 2.0f, sep_y}, {sz.width - margin - 2.0f, sep_y},
                                 preedit_brush_, 1.0f);
        if (ctx.theme) {
            preedit_brush_->SetColor(c2d(ctx.theme->preedit_text));
        }
    };

    auto draw_border = [&]() {
        if (ctx.theme && cfg && cfg->border_width > 0 && border_brush_) {
            float bw = (float)cfg->border_width;
            float inset = bw / 2.0f;
            D2D1_ROUNDED_RECT rr = {
                D2D1::RectF(inset, inset, sz.width - inset, sz.height - inset),
                (float)cfg->round_corner_ex,
                (float)cfg->round_corner_ex,
            };
            render_target_->DrawRoundedRectangle(rr, border_brush_, bw);
        }
    };

    if (!ctx.rects || ctx.rects->empty()) {
        // No candidates — show preedit if available, otherwise placeholder
        if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left) {
            draw_preedit(ctx);
        } else {
            render_target_->DrawText(L"知意输入法", 5, fmt_left_, D2D1::RectF(0,0,sz.width,sz.height), preedit_brush_);
        }
        draw_preedit_separator();
        draw_border();
        render_target_->EndDraw(); return;
    }

    // Preedit
    if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left) {
        draw_preedit(ctx);
        draw_preedit_separator();
    }

    // Candidates
    for (const auto& cr : *ctx.rects) {
        int i = cr.index;
        bool hl = (i == ctx.highlighted);
        bool hv = ctx.hovered_target == CandidateHoverTarget::Candidate &&
                  i == ctx.hovered_candidate_index;
        D2D1_RECT_F hr = {(float)cr.highlight_rect.left, (float)cr.highlight_rect.top,
                          (float)cr.highlight_rect.right, (float)cr.highlight_rect.bottom};
        if (hl || hv) {
            D2D1_ROUNDED_RECT rr = {hr, corner, corner};
            render_target_->FillRoundedRectangle(rr, hl ? highlight_brush_ : hover_brush_);
        }
        // Label
        std::wstring label = candidate_label(i);
        D2D1_RECT_F lr = {(float)cr.label_rect.left, (float)cr.label_rect.top,
                          (float)cr.label_rect.right, (float)cr.label_rect.bottom};
        render_target_->DrawText(label.c_str(), (UINT32)label.length(), fmt_right_, lr,
                                 hl ? highlight_text_brush_ : label_brush_);
        // Text
        auto wt = dec(cr.text);
        D2D1_RECT_F tr = {(float)cr.text_rect.left, (float)cr.text_rect.top,
                          (float)cr.text_rect.right, (float)cr.text_rect.bottom};
        render_target_->DrawText(wt.c_str(), (UINT32)wt.length(), fmt_left_, tr,
                                 hl ? highlight_text_brush_ : text_brush_);
        if (!cr.comment.empty()) {
            auto wc = dec(cr.comment);
            D2D1_RECT_F comment_rect = {
                (float)cr.comment_rect.left, (float)cr.comment_rect.top,
                (float)cr.comment_rect.right, (float)cr.comment_rect.bottom,
            };
            auto* brush = hl ? highlight_text_brush_ : (hv ? text_brush_ : comment_brush_);
            render_target_->DrawText(wc.c_str(), (UINT32)wc.length(), fmt_left_,
                                    comment_rect, brush);
        }
        if (cr.recommended && cr.mark_rect.right > cr.mark_rect.left) {
            draw_sparkle(cr.mark_rect, ctx.sparkle_t, hl);
        }
    }

    // Page nav (always visible, grayed when disabled)
    if (ctx.page_total > 1 && fmt_small_) {
        float nc = corner > 2 ? corner-1 : 1;
        bool pe = (ctx.page_current > 1), ne = (ctx.page_current < ctx.page_total);
        // <
        {
            D2D1_RECT_F pr = {(float)ctx.prev_button_rect.left, (float)ctx.prev_button_rect.top,
                              (float)ctx.prev_button_rect.right, (float)ctx.prev_button_rect.bottom};
            bool h = pe && ctx.hovered_target == CandidateHoverTarget::PreviousPage;
            if (h) { D2D1_ROUNDED_RECT rr = {pr, nc, nc}; render_target_->FillRoundedRectangle(rr, highlight_brush_); }
            auto* b = h ? highlight_text_brush_ : (pe ? nav_brush_ : preedit_brush_);
            render_target_->DrawText(L"<", 1, fmt_small_, pr, b);
        }
        // >
        {
            D2D1_RECT_F nr = {(float)ctx.next_button_rect.left, (float)ctx.next_button_rect.top,
                              (float)ctx.next_button_rect.right, (float)ctx.next_button_rect.bottom};
            bool h = ne && ctx.hovered_target == CandidateHoverTarget::NextPage;
            if (h) { D2D1_ROUNDED_RECT rr = {nr, nc, nc}; render_target_->FillRoundedRectangle(rr, highlight_brush_); }
            auto* b = h ? highlight_text_brush_ : (ne ? nav_brush_ : preedit_brush_);
            render_target_->DrawText(L">", 1, fmt_small_, nr, b);
        }
    }

    draw_border();

    render_target_->EndDraw();
}

void D2DRenderer::draw_sparkle(const RECT& mark, float t, bool highlighted) {
    const COLORREF c = sparkle_color(highlighted);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(render_target_->CreateSolidColorBrush(
            D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f), &brush))) {
        return;
    }
    for (const SparkleStar& star : sparkle_stars(mark, t)) {
        if (star.r < 0.5f || star.alpha < 0.02f) continue;
        ID2D1PathGeometry* path = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            const auto pts = sparkle_points(star);
            sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush->SetOpacity(star.alpha);
            render_target_->FillGeometry(path, brush);
        }
        if (sink) sink->Release();
        if (path) path->Release();
    }
    brush->Release();
}

void D2DRenderer::resize(int w, int h) { if (render_target_) render_target_->Resize(D2D1::SizeU(w, h)); }

} // namespace cxxime
