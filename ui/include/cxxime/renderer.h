// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_RENDERER_H_
#define CXXIME_RENDERER_H_

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cxxime/render_context.h>

namespace cxxime {

class D2DRenderer {
public:
    bool initialize(HWND hwnd, const Theme& theme, UINT dpi);
    void finalize();
    void render(const RenderContext& ctx);
    void resize(int width, int height);

private:
    void draw_preedit(const RenderContext& ctx);
    // Laya recommendation: the gradient wash under the candidate, and the star over it.
    void draw_recommend_wash(const D2D1_ROUNDED_RECT& box, const RECT& mark, float t,
                             bool highlighted, float wash_opacity);
    void draw_sparkle(const RECT& mark, float t, bool highlighted);
    // Blue-to-purple from the bottom-left to the top-right of `area`.
    ID2D1LinearGradientBrush* make_recommend_gradient(const D2D1_RECT_F& area, const Color& from,
                                                      const Color& to, float opacity);

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* render_target_ = nullptr;
    ID2D1SolidColorBrush* text_brush_ = nullptr;
    ID2D1SolidColorBrush* comment_brush_ = nullptr;
    ID2D1SolidColorBrush* bg_brush_ = nullptr;
    ID2D1SolidColorBrush* highlight_brush_ = nullptr;
    ID2D1SolidColorBrush* highlight_text_brush_ = nullptr;
    ID2D1SolidColorBrush* hover_brush_ = nullptr;
    ID2D1SolidColorBrush* preedit_brush_ = nullptr;
    ID2D1SolidColorBrush* preedit_separator_brush_ = nullptr;
    ID2D1SolidColorBrush* preedit_active_back_brush_ = nullptr;
    ID2D1SolidColorBrush* preedit_active_border_brush_ = nullptr;
    ID2D1SolidColorBrush* preedit_cursor_brush_ = nullptr;
    ID2D1SolidColorBrush* label_brush_ = nullptr;
    ID2D1SolidColorBrush* nav_brush_ = nullptr;
    ID2D1SolidColorBrush* border_brush_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* fmt_left_ = nullptr;
    IDWriteTextFormat* fmt_right_ = nullptr;
    IDWriteTextFormat* fmt_preedit_ = nullptr;
    IDWriteTextFormat* fmt_small_ = nullptr;
    IDWriteTextFormat* fmt_gloss_ = nullptr;      // learning mode translations, cut with "…"
    IDWriteInlineObject* gloss_ellipsis_ = nullptr;
};

} // namespace cxxime
#endif
