// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_GDI_RENDERER_H_
#define CXXIME_GDI_RENDERER_H_

#include <windows.h>

#include <cxxime/render_context.h>

namespace cxxime {

class GdiRenderer {
public:
    void initialize(HWND hwnd, const Theme& theme, UINT dpi);
    void render(HDC hdc, const RECT& clip, const RenderContext& ctx);
    void finalize();

private:
    HWND hwnd_ = nullptr;
    HFONT hfont_ = nullptr;
    HFONT preedit_font_ = nullptr;
    HFONT nav_font_ = nullptr;
    HFONT gloss_font_ = nullptr;  // learning mode translations
    HBRUSH bg_brush_ = nullptr;
    HBRUSH hl_brush_ = nullptr;
    HBRUSH hover_brush_ = nullptr;
    COLORREF text_color_ = 0;
    COLORREF comment_color_ = 0;
    COLORREF hl_text_color_ = 0;
    COLORREF preedit_color_ = 0;
    COLORREF preedit_cursor_color_ = 0;
    COLORREF label_color_ = 0;
    COLORREF nav_color_ = 0;
};

} // namespace cxxime
#endif
