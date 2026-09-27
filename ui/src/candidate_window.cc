// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/candidate_window.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include <dwmapi.h>

#include <cxxime/config.h>
#include <cxxime/renderer.h>
#include <cxxime/window_position.h>

#include "dpi_awareness.h"
#include "gdi_renderer.h"

namespace cxxime {

class CandidateWindow::GdiRenderer : public cxxime::GdiRenderer {};
class CandidateWindow::D2DRenderer : public cxxime::D2DRenderer {};

namespace {

constexpr int kPreeditCursorMinimumWidthDips = 2;
constexpr int kPreeditCursorMaximumWidthDips = 20;
constexpr int kPreeditCursorHeightPercent = 80;
constexpr int kPreeditCursorBorderGapDips = 1;
constexpr int kPreeditCursorTextGapDips = 1;
constexpr int kPreeditCursorIdleAccentPercent = 65;
constexpr UINT_PTR kPreeditCursorEmphasisTimerId = 1;
constexpr UINT kPreeditCursorEmphasisDurationMs = 160;
constexpr int kCandidateCaretGapPx = 4;

bool system_high_contrast_enabled() {
    HIGHCONTRASTW high_contrast = {sizeof(high_contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0) &&
           (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

DWORD system_caret_width() {
    DWORD width = 1;
    if (!SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &width, 0) || width == 0) {
        return 1;
    }
    return width;
}

Color system_color(int index) {
    const COLORREF color = GetSysColor(index);
    return {GetRValue(color), GetGValue(color), GetBValue(color), 255};
}

Color blend_color(const Color& source, const Color& target, int target_percent) {
    const int source_percent = 100 - target_percent;
    const auto blend_channel = [&](uint8_t source_channel, uint8_t target_channel) {
        return static_cast<uint8_t>(
            (source_channel * source_percent + target_channel * target_percent + 50) / 100);
    };
    return {
        blend_channel(source.r, target.r),
        blend_channel(source.g, target.g),
        blend_channel(source.b, target.b),
        255,
    };
}

Theme theme_for_rendering(const Theme& configured, bool high_contrast) {
    if (!high_contrast) {
        return configured;
    }
    Theme result = configured;
    result.background = system_color(COLOR_WINDOW);
    result.text = system_color(COLOR_WINDOWTEXT);
    result.comment_text = system_color(COLOR_WINDOWTEXT);
    result.label_text = system_color(COLOR_WINDOWTEXT);
    result.preedit_text = system_color(COLOR_WINDOWTEXT);
    result.preedit_separator = system_color(COLOR_WINDOWTEXT);
    result.preedit_active_back = system_color(COLOR_HIGHLIGHT);
    result.preedit_active_border = system_color(COLOR_HIGHLIGHTTEXT);
    result.preedit_cursor = system_color(COLOR_WINDOWTEXT);
    result.hilited_text = system_color(COLOR_HIGHLIGHTTEXT);
    result.hilited_back = system_color(COLOR_HIGHLIGHT);
    result.border = system_color(COLOR_WINDOWTEXT);
    result.prev_page = system_color(COLOR_WINDOWTEXT);
    result.next_page = system_color(COLOR_WINDOWTEXT);
    return result;
}

std::wstring utf8_to_wstring(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), &result[0], length) != length) {
        return {};
    }
    return result;
}

std::size_t clamp_utf8_boundary(const std::string& text, std::size_t offset) {
    offset = (std::min)(offset, text.size());
    while (offset > 0 && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
        --offset;
    }
    return offset;
}

int measure_text_width(HDC hdc, HFONT font, const std::string& text) {
    const std::wstring wide = utf8_to_wstring(text);
    if (wide.empty()) {
        return 0;
    }
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE size = {};
    GetTextExtentPoint32W(hdc, wide.c_str(), static_cast<int>(wide.size()), &size);
    SelectObject(hdc, old);
    return size.cx;
}

void append_preedit_runs(RenderContext& context, HDC hdc, HFONT font, const std::string& text,
                         PreeditRunKind kind, bool has_syllable_boundaries, bool focused, int top,
                         int height, int& x) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t separator = !has_syllable_boundaries || kind == PreeditRunKind::Converted
                                          ? std::string::npos
                                          : text.find('\'', begin);
        const std::size_t end = separator == std::string::npos ? text.size() : separator;
        if (end > begin) {
            PreeditTextRun run;
            run.text = text.substr(begin, end - begin);
            run.kind = kind;
            run.focused = focused;
            const int width = measure_text_width(hdc, font, run.text);
            run.rect = {x, top, x + width + (std::max)(2, height / 4), top + height};
            context.preedit_runs.push_back(std::move(run));
            x += width;
        }
        if (separator == std::string::npos) {
            break;
        }
        PreeditTextRun run;
        run.text = "'";
        run.kind = PreeditRunKind::Separator;
        run.focused = focused;
        const int width = measure_text_width(hdc, font, run.text);
        run.rect = {x, top, x + width + (std::max)(2, height / 4), top + height};
        context.preedit_runs.push_back(std::move(run));
        x += width;
        begin = separator + 1;
    }
}

void insert_preedit_cursor_slot(RenderContext& context, HDC hdc, HFONT font, std::size_t cursor,
                                int caret_x, int slot_width) {
    std::vector<PreeditTextRun> adjusted;
    adjusted.reserve(context.preedit_runs.size() + 1);
    std::size_t run_begin = 0;
    bool inserted = false;

    for (const PreeditTextRun& original : context.preedit_runs) {
        const std::size_t run_end = run_begin + original.text.size();
        if (inserted) {
            PreeditTextRun shifted = original;
            shifted.rect.left += slot_width;
            shifted.rect.right += slot_width;
            adjusted.push_back(std::move(shifted));
        } else if (cursor == run_begin) {
            inserted = true;
            PreeditTextRun shifted = original;
            shifted.rect.left += slot_width;
            shifted.rect.right += slot_width;
            adjusted.push_back(std::move(shifted));
        } else if (cursor > run_begin && cursor < run_end) {
            inserted = true;
            const std::size_t split = cursor - run_begin;
            PreeditTextRun prefix = original;
            prefix.text = original.text.substr(0, split);
            prefix.rect.right = prefix.rect.left + measure_text_width(hdc, font, prefix.text);
            adjusted.push_back(std::move(prefix));

            PreeditTextRun suffix = original;
            suffix.text = original.text.substr(split);
            suffix.rect.left = caret_x + slot_width;
            const int run_height = static_cast<int>(suffix.rect.bottom - suffix.rect.top);
            suffix.rect.right = suffix.rect.left + measure_text_width(hdc, font, suffix.text) +
                                (std::max)(2, run_height / 4);
            adjusted.push_back(std::move(suffix));
        } else {
            adjusted.push_back(original);
        }
        run_begin = run_end;
    }
    context.preedit_runs = std::move(adjusted);
}

} // namespace

CandidateWindow::~CandidateWindow() {
    destroy();
}

bool CandidateWindow::create(HWND owner, const Config& config) {
    if (hwnd_ && IsWindow(hwnd_)) {
        config_ = &config;
        return true;
    }
    if (hwnd_) {
        destroy();
    }

    config_ = &config;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"CxxIMECandidateWindow";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    ScopedDpiAwarenessContext dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"CxxIMECandidateWindow", L"", WS_POPUP, 0, 0, 300, 30,
                            owner, nullptr, GetModuleHandle(nullptr), this);
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        theme_ = build_theme_from_config(config);
        render_ctx_.high_contrast = system_high_contrast_enabled();
        render_theme_ = theme_for_rendering(theme_, render_ctx_.high_contrast);
        if (config.render_backend != "gdi") set_render_backend(RenderBackend::D2D);
        dpi_scale_ = GetDpiForWindow(hwnd_) / 96.0f;
        if (dpi_scale_ <= 0.0f) {
            dpi_scale_ = 1.0f;
        }
        refresh_preedit_cursor_width();
        init_gdi_renderer();
    }
    return hwnd_ != nullptr;
}

bool CandidateWindow::ensure_created(HWND owner) {
    if (owner && !IsWindow(owner)) {
        return false;
    }
    if (!is_created()) {
        if (!config_) {
            return false;
        }
        const Config* config = config_;
        if (hwnd_) {
            destroy();
        }
        if (!create(owner, *config)) {
            return false;
        }
    }
    const bool was_visible = is_visible();
    if (!owner_matches(owner) && was_visible) {
        hide();
    }
    set_owner(owner);
    if (was_visible && owner_matches(owner)) {
        show();
    }
    return is_created() && owner_matches(owner);
}

bool CandidateWindow::ensure_created_with_ownerless_fallback(HWND preferred_owner,
                                                             bool* ownerless) {
    if (ownerless) {
        *ownerless = false;
    }
    if (ensure_created(preferred_owner)) {
        return true;
    }

    // A normal-integrity server cannot always bind its popup to an elevated host.
    // Keep the candidate available as an ownerless topmost window in that case.
    if (!preferred_owner || !config_) {
        return false;
    }
    const Config* config = config_;
    set_owner(nullptr);
    if (!is_created() && !create(nullptr, *config)) {
        return false;
    }
    if (!owner_matches(nullptr)) {
        return false;
    }
    if (ownerless) {
        *ownerless = true;
    }
    return true;
}

void CandidateWindow::init_gdi_renderer() {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    gdi_renderer_ = new GdiRenderer();
    gdi_renderer_->initialize(hwnd_, render_theme_, GetDpiForWindow(hwnd_));
}
void CandidateWindow::init_d2d_renderer() {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    d2d_renderer_ = new D2DRenderer();
    if (!d2d_renderer_->initialize(hwnd_, render_theme_, GetDpiForWindow(hwnd_))) {
        delete d2d_renderer_;
        d2d_renderer_ = nullptr;
        backend_ = RenderBackend::GDI;
    }
}

bool CandidateWindow::refresh_dpi_scale() {
    if (!hwnd_)
        return false;

    float next_scale = GetDpiForWindow(hwnd_) / 96.0f;
    if (next_scale <= 0.0f)
        next_scale = 1.0f;
    if (std::fabs(next_scale - dpi_scale_) < 0.01f)
        return false;

    dpi_scale_ = next_scale;
    return true;
}

bool CandidateWindow::refresh_preedit_cursor_width() {
    const UINT window_dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    const int minimum_width =
        (std::max)(1, MulDiv(kPreeditCursorMinimumWidthDips, static_cast<int>(window_dpi), 96));
    int maximum_width = (std::max)(minimum_width, MulDiv(kPreeditCursorMaximumWidthDips,
                                                         static_cast<int>(window_dpi), 96));
    const int display_width = monitor_display_width();
    if (display_width > 0) {
        maximum_width =
            (std::max)(minimum_width,
                       (std::min)(maximum_width, (std::max)(1, display_width / 4)));
    }
    const DWORD requested_width = system_caret_width();
    const int bounded_system_width = requested_width > static_cast<DWORD>(maximum_width)
                                         ? maximum_width
                                         : static_cast<int>(requested_width);
    const int next_width = (std::max)(bounded_system_width, minimum_width);
    if (next_width == preedit_cursor_width_) {
        return false;
    }
    preedit_cursor_width_ = next_width;
    return true;
}

void CandidateWindow::clear_preedit_cursor_emphasis() {
    if (hwnd_) {
        KillTimer(hwnd_, kPreeditCursorEmphasisTimerId);
    }
    preedit_cursor_emphasized_ = false;
    render_ctx_.preedit_cursor_emphasized = false;
}

void CandidateWindow::recreate_renderers_for_dpi() {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    refresh_preedit_cursor_width();
    if (gdi_renderer_) {
        gdi_renderer_->finalize();
        gdi_renderer_->initialize(hwnd_, render_theme_, GetDpiForWindow(hwnd_));
    }
    if (d2d_renderer_) {
        d2d_renderer_->finalize();
        delete d2d_renderer_;
        d2d_renderer_ = nullptr;
        if (backend_ == RenderBackend::D2D)
            init_d2d_renderer();
    }
}

void CandidateWindow::destroy() {
    skip_next_show_redraw_ = false;
    clear_preedit_cursor_emphasis();
    if (gdi_renderer_) { gdi_renderer_->finalize(); delete gdi_renderer_; gdi_renderer_ = nullptr; }
    if (d2d_renderer_) { d2d_renderer_->finalize(); delete d2d_renderer_; d2d_renderer_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    window_width_ = 0;
    window_height_ = 0;
    window_corner_ = -1;
    visible_candidate_count_ = 0;
    reset_placement();
}

bool CandidateWindow::is_created() const {
    return hwnd_ && IsWindow(hwnd_);
}

void CandidateWindow::show() {
    if (!is_created()) {
        skip_next_show_redraw_ = false;
        return;
    }

    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    const bool was_visible_before_show = IsWindowVisible(hwnd_) != FALSE;
    const bool skip_redraw = skip_next_show_redraw_ && was_visible_before_show;

    if (!IsWindowVisible(hwnd_) && has_last_caret_rect_) {
        RECT wr = {};
        GetWindowRect(hwnd_, &wr);
        POINT target = {};
        if (calculate_target_position(last_caret_rect_,
                                      wr.right - wr.left,
                                      wr.bottom - wr.top,
                                      target)) {
            move_window_now(target.x, target.y);
        }
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_candidate_count_ = static_cast<int>(candidate_rects_.size());
    const bool redraw = !skip_redraw;
    skip_next_show_redraw_ = false;
    if (redraw) {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}
void CandidateWindow::hide() {
    skip_next_show_redraw_ = false;
    clear_preedit_cursor_emphasis();
    if (hwnd_ && IsWindowVisible(hwnd_))
        ShowWindow(hwnd_, SW_HIDE);
    set_owner(nullptr);
    visible_candidate_count_ = 0;
}

void CandidateWindow::reset_placement() {
    placement_side_ = CandidatePlacementSide::Unset;
    placement_monitor_ = nullptr;
    has_last_caret_rect_ = false;
    last_caret_rect_ = {};
}

void CandidateWindow::set_owner(HWND owner) {
    skip_next_show_redraw_ = false;
    if (!hwnd_ || (owner && !IsWindow(owner))) {
        return;
    }

    HWND actual_owner = GetWindow(hwnd_, GW_OWNER);
    HWND root_owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    if (actual_owner == owner || (root_owner && actual_owner == root_owner)) {
        return;
    }

    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    actual_owner = GetWindow(hwnd_, GW_OWNER);
    if (actual_owner == owner || (root_owner && actual_owner == root_owner)) {
        return;
    }

    if (config_) {
        const Config* config = config_;
        destroy();
        create(owner, *config);
    }
}
bool CandidateWindow::is_visible() const {
    if (!is_created() || IsWindowVisible(hwnd_) == FALSE) {
        return false;
    }
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
           cloaked == 0;
}
bool CandidateWindow::owner_matches(HWND owner) const {
    if (!is_created() || (owner && !IsWindow(owner))) {
        return false;
    }

    HWND actual_owner = GetWindow(hwnd_, GW_OWNER);
    HWND root_owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    return actual_owner == owner || (root_owner && actual_owner == root_owner);
}
int CandidateWindow::visible_candidate_count() const {
    return visible_candidate_count_;
}
SIZE CandidateWindow::window_size() const {
    RECT rect = {};
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    if (!hwnd_ || !GetWindowRect(hwnd_, &rect)) {
        return {};
    }
    return {rect.right - rect.left, rect.bottom - rect.top};
}
SIZE CandidateWindow::layout_size() const {
    return {window_width_, window_height_};
}
UINT CandidateWindow::dpi() const {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    return hwnd_ ? GetDpiForWindow(hwnd_) : 0;
}
bool CandidateWindow::get_window_rect(RECT* rect) const {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    return rect && hwnd_ && IsWindow(hwnd_) && GetWindowRect(hwnd_, rect) != FALSE;
}
void CandidateWindow::set_config(const Config& config) {
    config_ = &config;
    refresh_preedit_cursor_width();
    set_theme(build_theme_from_config(config));
    RenderBackend next_backend = config.render_backend != "gdi" ? RenderBackend::D2D : RenderBackend::GDI;
    if (d2d_renderer_) {
        d2d_renderer_->finalize();
        delete d2d_renderer_;
        d2d_renderer_ = nullptr;
    }
    set_render_backend(next_backend);
}
void CandidateWindow::set_theme(const Theme& t) {
    skip_next_show_redraw_ = false;
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    theme_ = t;
    render_ctx_.high_contrast = system_high_contrast_enabled();
    render_theme_ = theme_for_rendering(theme_, render_ctx_.high_contrast);
    render_ctx_.preedit_cursor_idle = blend_color(
        render_theme_.preedit_text, render_theme_.preedit_cursor,
        kPreeditCursorIdleAccentPercent);
    if (gdi_renderer_) {
        gdi_renderer_->finalize();
        gdi_renderer_->initialize(hwnd_, render_theme_, GetDpiForWindow(hwnd_));
    }
    if (hwnd_)
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}
void CandidateWindow::set_render_backend(RenderBackend b) {
    skip_next_show_redraw_ = false;
    backend_ = b;
    if (b == RenderBackend::D2D && !d2d_renderer_) init_d2d_renderer();
}
void CandidateWindow::set_page_info(int cur, int tot) {
    skip_next_show_redraw_ = false;
    page_current_ = cur;
    page_total_ = tot;
}
void CandidateWindow::set_preedit(const std::string& preedit) {
    set_preedit(preedit, preedit.size());
}

void CandidateWindow::set_preedit(const std::string& preedit, size_t cursor) {
    set_preedit(preedit, cursor, 0, preedit.size(), preedit.size(), false);
}

void CandidateWindow::set_preedit(const std::string& preedit, size_t cursor,
                                  size_t converted_prefix, size_t focused_start,
                                  size_t focused_end, bool has_syllable_boundaries) {
    skip_next_show_redraw_ = false;
    const size_t next_cursor = clamp_utf8_boundary(preedit, cursor);
    const bool text_changed = preedit != preedit_text_;
    const bool cursor_moved = !text_changed && next_cursor != preedit_cursor_;
    if (text_changed) {
        clear_preedit_cursor_emphasis();
    } else if (cursor_moved) {
        preedit_cursor_emphasized_ =
            hwnd_ && SetTimer(hwnd_, kPreeditCursorEmphasisTimerId,
                              kPreeditCursorEmphasisDurationMs, nullptr) != 0;
    }
    preedit_text_ = preedit;
    preedit_cursor_ = next_cursor;
    converted_prefix_ = clamp_utf8_boundary(preedit, converted_prefix);
    focused_preedit_start_ = clamp_utf8_boundary(preedit, focused_start);
    focused_preedit_end_ = clamp_utf8_boundary(preedit, focused_end);
    preedit_has_syllable_boundaries_ = has_syllable_boundaries;
    if (focused_preedit_start_ < converted_prefix_ ||
        focused_preedit_end_ < focused_preedit_start_) {
        focused_preedit_start_ = preedit.size();
        focused_preedit_end_ = preedit.size();
    } else if (focused_preedit_start_ == 0 && focused_preedit_end_ == 0 &&
               !preedit.empty()) {
        focused_preedit_start_ = preedit.size();
        focused_preedit_end_ = preedit.size();
    }
}
void CandidateWindow::set_layout(const std::string& l) {
    skip_next_show_redraw_ = false;
    layout_orientation_ = l;
}
void CandidateWindow::set_candidate_selection_callback(CandidateSelectionCallback cb) {
    candidate_selection_cb_ = std::move(cb);
}
void CandidateWindow::set_layout_changed_callback(LayoutChangedCallback cb) {
    layout_changed_cb_ = std::move(cb);
}
void CandidateWindow::set_page_callback(PageCallback cb) { page_cb_ = std::move(cb); }
void CandidateWindow::set_draggable(bool draggable) { draggable_ = draggable; }

void CandidateWindow::move_window_now(int x, int y) {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool CandidateWindow::calculate_target_position(const RECT& caret_rect, int width, int height,
                                                POINT& target) {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    HMONITOR hMon = MonitorFromRect(&caret_rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfo(hMon, &mi))
        return false;

    if (placement_monitor_ != hMon) {
        placement_side_ = CandidatePlacementSide::Unset;
        placement_monitor_ = hMon;
    }

    const CandidateWindowPlacement placement = calculate_candidate_window_position(
        caret_rect, width, height, kCandidateCaretGapPx, mi.rcMonitor, placement_side_);
    placement_side_ = placement.side;
    target = placement.position;
    return true;
}

int CandidateWindow::monitor_display_width() const {
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    HMONITOR monitor = nullptr;
    if (has_last_caret_rect_) {
        monitor = MonitorFromRect(&last_caret_rect_, MONITOR_DEFAULTTONEAREST);
    } else {
        HWND foreground = GetForegroundWindow();
        monitor = MonitorFromWindow(foreground ? foreground : hwnd_, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO info = {sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return 0;
    }
    return info.rcMonitor.right - info.rcMonitor.left;
}

void CandidateWindow::update_window_region(int width, int height, int corner) {
    if (!hwnd_ || width <= 0 || height <= 0)
        return;
    if (width == window_width_ && height == window_height_ && corner == window_corner_)
        return;

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, corner, corner);
    if (region) {
        if (SetWindowRgn(hwnd_, region, TRUE)) {
            window_width_ = width;
            window_height_ = height;
            window_corner_ = corner;
        } else {
            DeleteObject(region);
        }
    }
}

void CandidateWindow::move_to_caret(const RECT& caretRect) {
    if (!hwnd_) return;
    skip_next_show_redraw_ = false;

    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));

    has_last_caret_rect_ = true;
    last_caret_rect_ = caretRect;

    RECT wr = {};
    GetWindowRect(hwnd_, &wr);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;

    POINT target = {};
    if (!calculate_target_position(caretRect, ww, wh, target))
        return;

    move_window_now(target.x, target.y);
    if (refresh_dpi_scale()) {
        recreate_renderers_for_dpi();
        update(page_);
    }
}

void CandidateWindow::move_to_screen_position(int x, int y) {
    if (!hwnd_) {
        return;
    }
    skip_next_show_redraw_ = false;
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    move_window_now(x, y);
    if (refresh_dpi_scale()) {
        recreate_renderers_for_dpi();
        update(page_);
    }
}

void CandidateWindow::rebuild_render_context(const LayoutConfig& cfg, int window_width) {
    render_ctx_.rects = &candidate_rects_;
    render_ctx_.theme = &render_theme_;
    render_ctx_.layout_cfg = &cfg;
    render_ctx_.preedit = preedit_text_;
    render_ctx_.preedit_cursor = preedit_cursor_;
    render_ctx_.preedit_cursor_emphasized = preedit_cursor_emphasized_;
    render_ctx_.preedit_cursor_idle = blend_color(
        render_theme_.preedit_text, render_theme_.preedit_cursor,
        kPreeditCursorIdleAccentPercent);
    render_ctx_.page_current = page_current_;
    render_ctx_.page_total = page_total_;
    render_ctx_.highlighted = page_.candidates.empty() ? -1 : page_.highlighted;

    // Page nav placement depends on layout orientation
    render_ctx_.prev_button_rect = {};
    render_ctx_.next_button_rect = {};
    if (page_total_ > 1 && !candidate_rects_.empty()) {
        auto& last = candidate_rects_.back();
        const PageNavigationMetrics nav = candidate_page_navigation_metrics(dpi());
        const int pw = nav.button_width;
        const int nw = nav.button_width;
        int nav_h = last.highlight_rect.bottom - last.highlight_rect.top;

        if (layout_orientation_ == "vertical") {
            // Vertical layout: nav buttons at bottom-left, use text row height (no highlight padding)
            nav_h = last.text_rect.bottom - last.text_rect.top;
            int nav_y = last.text_rect.bottom;
            int nav_x = cfg.margin_x;
            render_ctx_.prev_button_rect = {nav_x, nav_y, nav_x + pw, nav_y + nav_h};
            render_ctx_.next_button_rect = {nav_x + pw + nav.gap, nav_y,
                                            nav_x + pw + nav.gap + nw, nav_y + nav_h};
            render_ctx_.page_indicator_rect = {};
        } else {
            // Horizontal layout: nav buttons after last candidate, same row
            int nav_y = last.highlight_rect.top;
            int x = last.highlight_rect.right + nav.leading_gap;
            render_ctx_.prev_button_rect = {x, nav_y, x + pw, nav_y + nav_h};
            render_ctx_.next_button_rect = {x + pw + nav.gap, nav_y,
                                            x + pw + nav.gap + nw, nav_y + nav_h};
            render_ctx_.page_indicator_rect = {};
        }
    }
}

void CandidateWindow::update(const CandidatePresentationPage& presentation) {
    CandidatePage page;
    page.page_index = presentation.page_index;
    page.page_offset = presentation.page_offset;
    page.page_size = presentation.page_size;
    page.extent = presentation.extent;
    page.highlighted = presentation.highlighted;
    page.candidates.reserve(presentation.items.size());
    for (const CandidatePresentationItem& item : presentation.items) {
        Candidate candidate;
        candidate.text = item.text;
        candidate.comment = item.hint;
        page.candidates.push_back(std::move(candidate));
    }
    update(page);
}

void CandidateWindow::update(const CandidatePage& page) {
    if (!hwnd_) {
        skip_next_show_redraw_ = false;
        return;
    }
    skip_next_show_redraw_ = IsWindowVisible(hwnd_) != FALSE;
    ScopedDpiAwarenessContext dpi_context(GetWindowDpiAwarenessContext(hwnd_));
    if (refresh_dpi_scale())
        recreate_renderers_for_dpi();

    page_ = page;
    candidate_rects_.clear();

    // Apply DPI scaling to pixel values (like Weasel's Layout constructor)
    scaled_cfg_ = config_->layout_config;
    float s = dpi_scale_;
    scaled_cfg_.margin_x = (int)(scaled_cfg_.margin_x * s);
    scaled_cfg_.margin_y = (int)(scaled_cfg_.margin_y * s);
    scaled_cfg_.spacing = (int)(scaled_cfg_.spacing * s);
    scaled_cfg_.candidate_spacing = (int)(scaled_cfg_.candidate_spacing * s);
    scaled_cfg_.hilite_padding_x = (int)(scaled_cfg_.hilite_padding_x * s);
    scaled_cfg_.hilite_padding_y = (int)(scaled_cfg_.hilite_padding_y * s);
    scaled_cfg_.round_corner = (int)(scaled_cfg_.round_corner * s);
    scaled_cfg_.round_corner_ex = (int)(scaled_cfg_.round_corner_ex * s);
    scaled_cfg_.border_width = (int)(scaled_cfg_.border_width * s);
    scaled_cfg_.preedit_highlight_padding_x =
        (int)(scaled_cfg_.preedit_highlight_padding_x * s);
    scaled_cfg_.preedit_highlight_padding_y =
        (int)(scaled_cfg_.preedit_highlight_padding_y * s);
    scaled_cfg_.preedit_confirmed_gap = (int)(scaled_cfg_.preedit_confirmed_gap * s);
    scaled_cfg_.preedit_boundary_gap = (int)(scaled_cfg_.preedit_boundary_gap * s);
    scaled_cfg_.preedit_highlight_corner = (int)(scaled_cfg_.preedit_highlight_corner * s);
    scaled_cfg_.preedit_highlight_border_width =
        (int)(scaled_cfg_.preedit_highlight_border_width * s);
    scaled_cfg_.min_width = (int)(scaled_cfg_.min_width * s);
    scaled_cfg_.max_width = (int)(scaled_cfg_.max_width * s);
    scaled_cfg_.max_height = (int)(scaled_cfg_.max_height * s);
    int display_width = monitor_display_width();
    if (display_width > 0) {
        int window_width_limit = display_width;
        if (scaled_cfg_.max_width <= 0) {
            window_width_limit = calculate_auto_candidate_window_max_width(display_width, s);
        }
        int layout_width =
            (std::max)(1, window_width_limit - scaled_cfg_.border_width * 2);
        if (scaled_cfg_.max_width <= 0 || scaled_cfg_.max_width > layout_width) {
            scaled_cfg_.max_width = layout_width;
        }
        if (scaled_cfg_.min_width > layout_width) {
            scaled_cfg_.min_width = layout_width;
        }
    }
    auto& cfg = scaled_cfg_;
    HDC hdc = GetDC(hwnd_);
    const UINT window_dpi = GetDpiForWindow(hwnd_);
    auto calculate_layout = [&]() {
        if (layout_orientation_ == "horizontal") {
            return calculate_horizontal_layout(
                hdc, page.candidates, config_->font_name, config_->font_size, cfg, page_total_,
                window_dpi);
        }
        return calculate_vertical_layout(
            hdc, page.candidates, config_->font_name, config_->font_size, cfg, window_dpi);
    };
    LayoutResult lr = calculate_layout();
    if (page.extent.known_count > 0 ||
        page.extent.state != CandidateExtentState::kExhausted) {
        int visible_count = static_cast<int>(lr.rects.size());
        bool has_next = candidate_extent_may_continue(
            page.extent, page.page_offset + visible_count);
        int adjusted_page_total = page_current_ + (has_next ? 1 : 0);
        bool nav_visibility_changed = (page_total_ > 1) != (adjusted_page_total > 1);
        page_total_ = adjusted_page_total;
        if (nav_visibility_changed) {
            lr = calculate_layout();
        }
    }

    render_ctx_.preedit_runs.clear();
    render_ctx_.preedit_active_rect = {};
    render_ctx_.preedit_cursor_rect = {};
    render_ctx_.preedit_cursor_in_focus = false;
    const bool high_contrast = system_high_contrast_enabled();
    if (render_ctx_.high_contrast != high_contrast) {
        render_ctx_.high_contrast = high_contrast;
        render_theme_ = theme_for_rendering(theme_, high_contrast);
        if (gdi_renderer_) {
            gdi_renderer_->finalize();
            gdi_renderer_->initialize(hwnd_, render_theme_, window_dpi);
        }
    } else {
        render_theme_ = theme_for_rendering(theme_, high_contrast);
    }
    const int preedit_padding_x = (std::max)(0, cfg.preedit_highlight_padding_x);
    const int preedit_padding_y = (std::max)(0, cfg.preedit_highlight_padding_y);
    const int converted_active_gap = (std::max)(0, cfg.preedit_confirmed_gap);
    const int focused_boundary_gap = (std::max)(0, cfg.preedit_boundary_gap);
    render_ctx_.preedit_corner_radius = (std::max)(0, cfg.preedit_highlight_corner);
    render_ctx_.preedit_border_width =
        (std::max)(0, cfg.preedit_highlight_border_width);
    const int preedit_cursor_border_gap = (std::max)(
        1, MulDiv(kPreeditCursorBorderGapDips, static_cast<int>(window_dpi), 96));
    const int preedit_cursor_text_gap = (std::max)(
        1, MulDiv(kPreeditCursorTextGapDips, static_cast<int>(window_dpi), 96));
    const int preedit_cursor_slot_width =
        preedit_cursor_width_ + preedit_cursor_text_gap * 2;
    const int preedit_cursor_safe_inset =
        (render_ctx_.preedit_border_width + 1) / 2 + preedit_cursor_border_gap;
    const int preedit_cursor_horizontal_safe_inset =
        preedit_cursor_safe_inset + render_ctx_.preedit_corner_radius;
    const int focused_padding_left =
        (std::max)(preedit_padding_x, preedit_cursor_horizontal_safe_inset);
    const int focused_padding_right =
        (std::max)(preedit_padding_x, preedit_cursor_horizontal_safe_inset);

    // Preedit layout is measured once and shared by GDI and D2D renderers.
    if (!preedit_text_.empty()) {
        HFONT hf = CreateFontW(-MulDiv(theme_.preedit_font_size,
                                      window_dpi, 72),
                               0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, theme_.font_name.c_str());
        TEXTMETRICW metrics = {};
        int text_height = lr.row_height;
        if (hf) {
            HFONT old = (HFONT)SelectObject(hdc, hf);
            if (GetTextMetricsW(hdc, &metrics) && metrics.tmHeight > 0) {
                text_height = metrics.tmHeight;
            }
            SelectObject(hdc, old);
        }
        const int row_h = lr.row_height > 0 ? lr.row_height : text_height;
        const int content_height = (std::max)(text_height + preedit_padding_y * 2, row_h);
        const int text_top = cfg.margin_y + (content_height - text_height) / 2;
        int x = cfg.margin_x;

        const std::size_t converted = (std::min)(converted_prefix_, preedit_text_.size());
        const std::size_t focused_start =
            (std::max)(converted, (std::min)(focused_preedit_start_, preedit_text_.size()));
        const std::size_t focused_end =
            (std::max)(focused_start, (std::min)(focused_preedit_end_, preedit_text_.size()));
        const std::string converted_text = preedit_text_.substr(0, converted);
        if (!converted_text.empty()) {
            append_preedit_runs(render_ctx_, hdc, hf, converted_text,
                                PreeditRunKind::Converted, preedit_has_syllable_boundaries_, false,
                                text_top, text_height, x);
            x += converted_active_gap;
        }

        const std::string before_focus = preedit_text_.substr(converted, focused_start - converted);
        append_preedit_runs(render_ctx_, hdc, hf, before_focus, PreeditRunKind::Active,
                            preedit_has_syllable_boundaries_, false, text_top, text_height, x);
        const int focus_left = x;
        const std::string focused_text =
            preedit_text_.substr(focused_start, focused_end - focused_start);
        if (!focused_text.empty()) {
            x += focused_padding_left;
            append_preedit_runs(render_ctx_, hdc, hf, focused_text, PreeditRunKind::Active,
                                preedit_has_syllable_boundaries_, true, text_top, text_height, x);
            render_ctx_.preedit_active_rect = {
                focus_left,
                text_top - preedit_padding_y,
                x + focused_padding_right,
                text_top + text_height + preedit_padding_y,
            };
            if (focused_end < preedit_text_.size()) {
                x = render_ctx_.preedit_active_rect.right + focused_boundary_gap;
            } else {
                x = render_ctx_.preedit_active_rect.right;
            }
        }
        const std::string after_focus = preedit_text_.substr(focused_end);
        append_preedit_runs(render_ctx_, hdc, hf, after_focus, PreeditRunKind::Active,
                            preedit_has_syllable_boundaries_, false, text_top, text_height, x);

        auto cursor_x = [&](std::size_t cursor) {
            cursor = (std::min)(cursor, preedit_text_.size());
            int position = cfg.margin_x;
            if (cursor < converted) {
                return position + measure_text_width(hdc, hf, preedit_text_.substr(0, cursor));
            }
            position += measure_text_width(hdc, hf, converted_text);
            if (!converted_text.empty()) {
                position += converted_active_gap;
            }
            if (cursor < focused_start) {
                return position + measure_text_width(
                                      hdc, hf, preedit_text_.substr(converted, cursor - converted));
            }
            position += measure_text_width(hdc, hf, before_focus);
            if (!focused_text.empty()) {
                position += focused_padding_left;
            }
            if (cursor <= focused_end) {
                return position + measure_text_width(
                    hdc, hf, preedit_text_.substr(focused_start, cursor - focused_start));
            }
            position += measure_text_width(hdc, hf, focused_text) + focused_padding_right +
                        focused_boundary_gap;
            return position + measure_text_width(
                                  hdc, hf, preedit_text_.substr(focused_end, cursor - focused_end));
        };
        const bool draw_preedit_cursor = preedit_cursor_ < preedit_text_.size();
        if (draw_preedit_cursor) {
            const int caret_x = cursor_x(preedit_cursor_);
            render_ctx_.preedit_cursor_in_focus =
                !focused_text.empty() && preedit_cursor_ >= focused_start &&
                preedit_cursor_ <= focused_end;
            int safe_top = text_top;
            int safe_bottom = text_top + text_height;
            if (render_ctx_.preedit_cursor_in_focus) {
                safe_top = (std::max)(safe_top,
                                        static_cast<int>(render_ctx_.preedit_active_rect.top) +
                                             preedit_cursor_safe_inset);
                safe_bottom = (std::min)(safe_bottom,
                                         static_cast<int>(render_ctx_.preedit_active_rect.bottom) -
                                             preedit_cursor_safe_inset);
            }
            const int available_height = safe_bottom - safe_top;
            const int desired_height = (std::max)(
                1, (text_height * kPreeditCursorHeightPercent + 50) / 100);
            if (available_height > 0) {
                const int cursor_height = (std::min)(desired_height, available_height);
                const int centered_top = text_top + (text_height - cursor_height) / 2;
                const int cursor_top = (std::clamp)(
                    centered_top, safe_top, safe_bottom - cursor_height);
                render_ctx_.preedit_cursor_rect = {
                    caret_x + preedit_cursor_text_gap,
                    cursor_top,
                    caret_x + preedit_cursor_text_gap + preedit_cursor_width_,
                    cursor_top + cursor_height,
                };
            }
            insert_preedit_cursor_slot(render_ctx_, hdc, hf, preedit_cursor_, caret_x,
                                      preedit_cursor_slot_width);
            x += preedit_cursor_slot_width;
            if (render_ctx_.preedit_cursor_in_focus) {
                render_ctx_.preedit_active_rect.right += preedit_cursor_slot_width;
            } else if (!focused_text.empty() && preedit_cursor_ < focused_start) {
                render_ctx_.preedit_active_rect.left += preedit_cursor_slot_width;
                render_ctx_.preedit_active_rect.right += preedit_cursor_slot_width;
            }
        }

        const int preedit_h = content_height + cfg.spacing;
        for (auto& cr : lr.rects) {
            cr.label_rect.top += preedit_h;       cr.label_rect.bottom += preedit_h;
            cr.text_rect.top += preedit_h;        cr.text_rect.bottom += preedit_h;
            cr.comment_rect.top += preedit_h;     cr.comment_rect.bottom += preedit_h;
            cr.highlight_rect.top += preedit_h;   cr.highlight_rect.bottom += preedit_h;
        }
        int preedit_w = x + cfg.margin_x;
        if (cfg.max_width > 0) {
            preedit_w = (std::min)(preedit_w, cfg.max_width);
        }
        if (preedit_w > lr.width) {
            lr.width = preedit_w;
        }
        const LONG preedit_clip_left = static_cast<LONG>(cfg.margin_x);
        const LONG preedit_clip_right = (std::max)(
            preedit_clip_left, static_cast<LONG>(lr.width - cfg.margin_x));
        auto clip_preedit_rect = [&](RECT& rect) {
            rect.left = (std::clamp)(rect.left, preedit_clip_left, preedit_clip_right);
            rect.right = (std::clamp)(rect.right, rect.left, preedit_clip_right);
        };
        clip_preedit_rect(render_ctx_.preedit_active_rect);
        if (render_ctx_.preedit_active_rect.right <= render_ctx_.preedit_active_rect.left) {
            render_ctx_.preedit_cursor_in_focus = false;
        }
        if (draw_preedit_cursor) {
            const LONG cursor_width = render_ctx_.preedit_cursor_rect.right -
                                      render_ctx_.preedit_cursor_rect.left;
            LONG cursor_clip_left = preedit_clip_left;
            LONG cursor_clip_right = preedit_clip_right;
            if (render_ctx_.preedit_cursor_in_focus) {
                cursor_clip_left = (std::max)(
                    cursor_clip_left,
                    render_ctx_.preedit_active_rect.left +
                        preedit_cursor_horizontal_safe_inset);
                cursor_clip_right = (std::min)(
                    cursor_clip_right,
                    render_ctx_.preedit_active_rect.right -
                        preedit_cursor_horizontal_safe_inset);
            }
            const LONG available_width = cursor_clip_right - cursor_clip_left;
            if (available_width <= 0 || cursor_width <= 0) {
                render_ctx_.preedit_cursor_rect = {};
            } else {
                render_ctx_.preedit_cursor_rect.left = (std::max)(
                    render_ctx_.preedit_cursor_rect.left, cursor_clip_left);
                render_ctx_.preedit_cursor_rect.right = (std::min)(
                    render_ctx_.preedit_cursor_rect.right, cursor_clip_right);
                if (render_ctx_.preedit_cursor_rect.right <=
                    render_ctx_.preedit_cursor_rect.left) {
                    render_ctx_.preedit_cursor_rect = {};
                }
            }
        }
        for (auto& run : render_ctx_.preedit_runs) {
            clip_preedit_rect(run.rect);
        }
        render_ctx_.preedit_rect = {cfg.margin_x, cfg.margin_y,
                                    lr.width - cfg.margin_x, cfg.margin_y + content_height};
        // Store preedit text height for separator positioning
        render_ctx_.preedit_text_height = content_height;
        lr.row_height = row_h;
        lr.height += preedit_h;
        if (hf) {
            DeleteObject(hf);
        }
    } else {
        render_ctx_.preedit_rect = {};
    }
    ReleaseDC(hwnd_, hdc);

    candidate_rects_ = std::move(lr.rects);
    visible_candidate_count_ = static_cast<int>(candidate_rects_.size());
    rebuild_render_context(cfg, lr.width);
    // Extend width for page nav buttons if present
    if (page_total_ > 1 && render_ctx_.next_button_rect.right > lr.width)
        lr.width = render_ctx_.next_button_rect.right + cfg.margin_x;
    // Vertical layout: extend height for nav buttons row below candidates
    if (layout_orientation_ == "vertical" && page_total_ > 1 && !candidate_rects_.empty()) {
        int nav_bottom = render_ctx_.next_button_rect.bottom + cfg.hilite_padding_y;
        if (nav_bottom > lr.height) lr.height = nav_bottom;
    }
    int border = cfg.border_width > 0 ? cfg.border_width : 0;
    if (border > 0) {
        lr.width += border * 2;
        lr.height += border * 2;
    }
    if (lr.width != window_width_ || lr.height != window_height_) {
        POINT target = {};
        bool moved_with_resize = false;
        if (IsWindowVisible(hwnd_) && has_last_caret_rect_ &&
            calculate_target_position(last_caret_rect_, lr.width, lr.height, target)) {
            SetWindowPos(hwnd_, nullptr, target.x, target.y, lr.width, lr.height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            moved_with_resize = true;
        }
        if (!moved_with_resize) {
            SetWindowPos(hwnd_, nullptr, 0, 0, lr.width, lr.height,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }
    if (d2d_renderer_) d2d_renderer_->resize(lr.width, lr.height);
    update_window_region(lr.width, lr.height, cfg.round_corner_ex);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

// --- WndProc ---
static POINT lp2pt(LPARAM lp) { POINT p; p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp); return p; }

LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
        if (self && self->gdi_renderer_) {
            if (self->backend_ == RenderBackend::D2D && self->d2d_renderer_)
                self->d2d_renderer_->render(self->render_ctx_);
            else
                self->gdi_renderer_->render(hdc, rc, self->render_ctx_);
        }
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!self || self->page_.candidates.empty()) return 0;
        if (self->draggable_) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        POINT pt = lp2pt(lp);
        if (PtInRect(&self->render_ctx_.prev_button_rect, pt) && self->page_current_ > 1) {
            if (self->page_cb_) {
                self->page_cb_(CandidatePageDirection::Previous);
            }
            return 0;
        }
        if (PtInRect(&self->render_ctx_.next_button_rect, pt) &&
            self->page_current_ < self->page_total_) {
            if (self->page_cb_) {
                self->page_cb_(CandidatePageDirection::Next);
            }
            return 0;
        }
        for (const auto& cr : self->candidate_rects_) {
            if (PtInRect(&cr.highlight_rect, pt)) {
                if (self->candidate_selection_cb_ && cr.index >= 0) {
                    self->candidate_selection_cb_(static_cast<std::size_t>(cr.index));
                }
                break;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self || self->page_.candidates.empty()) return 0;
        POINT pt = lp2pt(lp);
        CandidateHoverTarget hovered_target = CandidateHoverTarget::None;
        int hovered_candidate_index = -1;
        RECT old_r{}, new_r{};
        const CandidateHoverTarget old_target = self->render_ctx_.hovered_target;
        const int old_candidate_index = self->render_ctx_.hovered_candidate_index;
        // Find old hover rect for targeted invalidation
        auto find_rect = [&](CandidateHoverTarget target, int candidate_index) -> RECT {
            if (target == CandidateHoverTarget::Candidate) {
                for (const auto& cr : *self->render_ctx_.rects) {
                    if (cr.index == candidate_index) {
                        return cr.highlight_rect;
                    }
                }
            } else if (target == CandidateHoverTarget::PreviousPage) {
                return self->render_ctx_.prev_button_rect;
            } else if (target == CandidateHoverTarget::NextPage) {
                return self->render_ctx_.next_button_rect;
            }
            return {};
        };
        old_r = find_rect(old_target, old_candidate_index);

        if (PtInRect(&self->render_ctx_.prev_button_rect, pt)) {
            hovered_target = CandidateHoverTarget::PreviousPage;
            new_r = self->render_ctx_.prev_button_rect;
        } else if (PtInRect(&self->render_ctx_.next_button_rect, pt)) {
            hovered_target = CandidateHoverTarget::NextPage;
            new_r = self->render_ctx_.next_button_rect;
        } else {
            for (const auto& cr : self->candidate_rects_) {
                if (PtInRect(&cr.highlight_rect, pt)) {
                    hovered_target = CandidateHoverTarget::Candidate;
                    hovered_candidate_index = cr.index;
                    new_r = cr.highlight_rect;
                    break;
                }
            }
        }

        if (hovered_target != old_target || hovered_candidate_index != old_candidate_index) {
            self->render_ctx_.hovered_target = hovered_target;
            self->render_ctx_.hovered_candidate_index = hovered_candidate_index;
            if (old_r.right > old_r.left) InvalidateRect(hwnd, &old_r, FALSE);
            if (new_r.right > new_r.left) InvalidateRect(hwnd, &new_r, FALSE);
        }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self) {
            self->render_ctx_.hovered_target = CandidateHoverTarget::None;
            self->render_ctx_.hovered_candidate_index = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // prevent focus theft on click
    case WM_NCHITTEST: return HTCLIENT;  // prevent resize cursor at edges
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (self && wp == kPreeditCursorEmphasisTimerId) {
            const RECT cursor_rect = self->render_ctx_.preedit_cursor_rect;
            self->clear_preedit_cursor_emphasis();
            if (cursor_rect.right > cursor_rect.left) {
                InvalidateRect(hwnd, &cursor_rect, FALSE);
            }
        }
        return 0;
    case WM_DPICHANGED:
        if (self) {
            float next_scale = HIWORD(wp) / 96.0f;
            if (next_scale > 0.0f && std::fabs(next_scale - self->dpi_scale_) >= 0.01f) {
                self->dpi_scale_ = next_scale;
                RECT* suggested = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                self->recreate_renderers_for_dpi();
                self->update(self->page_);
                if (self->has_last_caret_rect_) {
                    RECT window_rect = {};
                    if (GetWindowRect(hwnd, &window_rect)) {
                        POINT target = {};
                        if (self->calculate_target_position(
                                self->last_caret_rect_, window_rect.right - window_rect.left,
                                window_rect.bottom - window_rect.top, target)) {
                            self->move_window_now(target.x, target.y);
                        }
                    }
                }
                if (self->layout_changed_cb_) {
                    self->layout_changed_cb_();
                }
            }
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (self) {
            self->refresh_preedit_cursor_width();
            self->update(self->page_);
            if (self->layout_changed_cb_) {
                self->layout_changed_cb_();
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace cxxime
