// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "voice_overlay.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>

#include <cxxime/render_context.h>

namespace {

constexpr UINT kUpdateMessage = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kHideTimer = 2;
constexpr UINT kFrameMs = 33;
constexpr UINT kMessageMs = 2200;
constexpr wchar_t kClassName[] = L"ZhiyuIMEVoiceWindow";

// Sizes in DIPs.
constexpr float kShadow = 10.0f;      // room around the bar for its shadow
constexpr float kBarHeight = 52.0f;
constexpr float kBarWidth = 236.0f;
constexpr float kButton = 34.0f;      // the microphone and stop circles
constexpr float kSideGap = 9.0f;      // from the bar's end to a circle
constexpr float kBottomGap = 72.0f;   // above the bottom of the work area
constexpr int kBars = 22;
constexpr float kTextSize = 13.0f;

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

D2D1_COLOR_F color(const cxxime::Color& c, float alpha = 1.0f) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha);
}

cxxime::Color mix(const cxxime::Color& a, const cxxime::Color& b, float amount) {
    auto channel = [&](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x * (1.0f - amount) + y * amount));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255};
}

struct Strings {
    const wchar_t* recognizing;
    const wchar_t* messages[9];  // VoiceOverlay::Message order
};

const Strings kChinese = {
    L"正在识别",
    {L"没有检测到人声", L"没有识别出文字", L"没有找到麦克风", L"麦克风权限已关闭（Windows 隐私设置）",
     L"麦克风无法使用", L"请先在设置中下载语音模型", L"语音输入未开启（设置 > 语音输入）",
     L"请重启这个程序后再使用语音输入", L"语音识别失败"},
};
const Strings kEnglish = {
    L"Recognizing",
    {L"No speech detected", L"No text recognized", L"No microphone found",
     L"Microphone access is off (Windows privacy settings)", L"The microphone cannot be used",
     L"Download the voice model in Settings first", L"Voice input is off (Settings > Voice input)",
     L"Restart this program to use voice input", L"Speech recognition failed"},
};

bool english_ui(const std::string& language) {
    if (language.rfind("en", 0) == 0) return true;
    if (language.rfind("zh", 0) == 0) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
}

}  // namespace

struct VoiceOverlay::Impl {
    enum class Mode { kHidden, kListening, kRecognizing, kMessage };

    std::thread thread;
    HWND hwnd = nullptr;
    std::function<void()> stop_clicked;
    std::mutex ready_mutex;
    std::condition_variable ready;
    bool started = false;

    // Shared with the callers; under `mutex`.
    std::mutex mutex;
    Mode mode = Mode::kHidden;
    Message message = Message::kNoSpeech;
    std::deque<float> levels;  // newest last; 0 while nobody speaks
    cxxime::Theme theme;
    bool english = false;
    bool shown_changed = false;  // the mode changed: placed again

    // The window's thread only.
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    HDC memory_dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    SIZE bitmap_size = {};
    std::array<float, kBars> shown_levels = {};
    float phase = 0.0f;
    bool hover = false;
    bool pressed = false;
    bool tracking = false;
    float scale = 1.0f;
    D2D1_POINT_2F stop_center = {};  // DIPs, window coordinates
    bool stop_visible = false;

    void run();
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void apply_state();
    void render();
    void free_bitmap();
    bool over_stop(LPARAM lp) const;
    void draw_microphone(D2D1_POINT_2F center, const D2D1_COLOR_F& back, const D2D1_COLOR_F& fore);
    float text_width(const std::wstring& text, IDWriteTextFormat* format) const;
};

VoiceOverlay::VoiceOverlay() : impl_(std::make_unique<Impl>()) {}
VoiceOverlay::~VoiceOverlay() { stop(); }

bool VoiceOverlay::start(std::function<void()> stop_clicked) {
    if (impl_->thread.joinable()) return true;
    impl_->stop_clicked = std::move(stop_clicked);
    impl_->started = false;
    impl_->thread = std::thread(&Impl::run, impl_.get());
    std::unique_lock<std::mutex> lock(impl_->ready_mutex);
    impl_->ready.wait(lock, [&] { return impl_->started; });
    return impl_->hwnd != nullptr;
}

void VoiceOverlay::stop() {
    if (!impl_->thread.joinable()) return;
    if (impl_->hwnd) PostMessageW(impl_->hwnd, WM_CLOSE, 0, 0);
    impl_->thread.join();
}

void VoiceOverlay::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    if (!config) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->theme = cxxime::build_theme_from_config(*config);
    impl_->english = english_ui(config->ui_language);
}

void VoiceOverlay::show_listening() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mode = Impl::Mode::kListening;
        impl_->levels.assign(kBars, 0.0f);
        impl_->shown_changed = true;
    }
    if (impl_->hwnd) PostMessageW(impl_->hwnd, kUpdateMessage, 0, 0);
}

void VoiceOverlay::add_level(float level, bool speaking) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->levels.push_back(speaking ? level : 0.0f);
    while (impl_->levels.size() > static_cast<std::size_t>(kBars)) impl_->levels.pop_front();
}

void VoiceOverlay::show_recognizing() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mode = Impl::Mode::kRecognizing;
        impl_->shown_changed = true;
    }
    if (impl_->hwnd) PostMessageW(impl_->hwnd, kUpdateMessage, 0, 0);
}

void VoiceOverlay::show_message(Message message) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mode = Impl::Mode::kMessage;
        impl_->message = message;
        impl_->shown_changed = true;
    }
    if (impl_->hwnd) PostMessageW(impl_->hwnd, kUpdateMessage, 0, 0);
}

void VoiceOverlay::hide() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mode = Impl::Mode::kHidden;
        impl_->shown_changed = true;
    }
    if (impl_->hwnd) PostMessageW(impl_->hwnd, kUpdateMessage, 0, 0);
}

void VoiceOverlay::Impl::run() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&dwrite));
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    hwnd = factory && dwrite
               ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                 kClassName, L"Zhiyu IME Voice", WS_POPUP, 0, 0, 0, 0, nullptr,
                                 nullptr, wc.hInstance, this)
               : nullptr;
    {
        std::lock_guard<std::mutex> lock(ready_mutex);
        started = true;
    }
    ready.notify_all();
    if (hwnd) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    free_bitmap();
    release(target);
    release(dwrite);
    release(factory);
}

LRESULT CALLBACK VoiceOverlay::Impl::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        // The window exists from here on: Impl::hwnd before CreateWindowExW returns, so the
        // messages of its creation go to it (not to a null window, which fails the creation).
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* impl = static_cast<Impl*>(create->lpCreateParams);
        impl->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

bool VoiceOverlay::Impl::over_stop(LPARAM lp) const {
    if (!stop_visible) return false;
    const float x = static_cast<float>(static_cast<short>(LOWORD(lp))) / scale;
    const float y = static_cast<float>(static_cast<short>(HIWORD(lp))) / scale;
    const float dx = x - stop_center.x, dy = y - stop_center.y;
    return dx * dx + dy * dy <= (kButton / 2 + 2) * (kButton / 2 + 2);
}

LRESULT VoiceOverlay::Impl::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kUpdateMessage:
        apply_state();
        return 0;
    case WM_TIMER:
        if (wp == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (mode == Mode::kMessage) mode = Mode::kHidden;
            }
            apply_state();
        } else if (wp == kAnimationTimer) {
            phase += kFrameMs / 1000.0f;
            render();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const bool now = over_stop(lp);
        if (now != hover) {
            hover = now;
            render();
        }
        if (!tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        hover = pressed = false;
        render();
        return 0;
    case WM_LBUTTONDOWN:
        pressed = over_stop(lp);
        if (pressed) SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        const bool clicked = pressed && over_stop(lp);
        pressed = false;
        ReleaseCapture();
        if (clicked && stop_clicked) stop_clicked();
        return 0;
    }
    case WM_SETCURSOR:
        if (hover) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void VoiceOverlay::Impl::apply_state() {
    Mode now;
    {
        std::lock_guard<std::mutex> lock(mutex);
        now = mode;
        shown_changed = false;
    }
    if (now == Mode::kHidden) {
        KillTimer(hwnd, kAnimationTimer);
        KillTimer(hwnd, kHideTimer);
        ShowWindow(hwnd, SW_HIDE);
        hover = pressed = false;
        return;
    }
    if (now == Mode::kMessage) {
        SetTimer(hwnd, kHideTimer, kMessageMs, nullptr);
    } else {
        KillTimer(hwnd, kHideTimer);
    }
    shown_levels.fill(0.0f);
    SetTimer(hwnd, kAnimationTimer, kFrameMs, nullptr);
    render();
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void VoiceOverlay::Impl::free_bitmap() {
    if (memory_dc) {
        if (old_bitmap) SelectObject(memory_dc, old_bitmap);
        DeleteDC(memory_dc);
    }
    if (bitmap) DeleteObject(bitmap);
    memory_dc = nullptr;
    bitmap = nullptr;
    old_bitmap = nullptr;
    bitmap_size = {};
}

float VoiceOverlay::Impl::text_width(const std::wstring& text, IDWriteTextFormat* format) const {
    IDWriteTextLayout* layout = nullptr;
    float width = 0.0f;
    if (SUCCEEDED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                           2000.0f, 100.0f, &layout))) {
        DWRITE_TEXT_METRICS metrics = {};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) width = metrics.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    return width;
}

// A microphone drawn in a filled circle: a capsule, its holder and a stand.
void VoiceOverlay::Impl::draw_microphone(D2D1_POINT_2F c, const D2D1_COLOR_F& back,
                                         const D2D1_COLOR_F& fore) {
    ID2D1SolidColorBrush* brush = nullptr;
    target->CreateSolidColorBrush(back, &brush);
    if (!brush) return;
    target->FillEllipse(D2D1::Ellipse(c, kButton / 2, kButton / 2), brush);
    brush->SetColor(fore);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(c.x - 3.2f, c.y - 9.0f, c.x + 3.2f, c.y + 2.5f), 3.2f, 3.2f), brush);
    ID2D1PathGeometry* path = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&path))) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(c.x - 6.5f, c.y - 2.0f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + 6.5f, c.y - 2.0f), D2D1::SizeF(6.5f, 6.5f),
                                          0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                          D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            target->DrawGeometry(path, brush, 1.6f);
        }
        path->Release();
    }
    target->DrawLine(D2D1::Point2F(c.x, c.y + 4.5f), D2D1::Point2F(c.x, c.y + 8.0f), brush, 1.6f);
    target->DrawLine(D2D1::Point2F(c.x - 3.5f, c.y + 8.0f), D2D1::Point2F(c.x + 3.5f, c.y + 8.0f), brush, 1.6f);
    brush->Release();
}

void VoiceOverlay::Impl::render() {
    Mode now;
    Message shown_message;
    cxxime::Theme colors;
    bool in_english;
    std::array<float, kBars> wanted = {};
    {
        std::lock_guard<std::mutex> lock(mutex);
        now = mode;
        shown_message = message;
        colors = theme;
        in_english = english;
        for (std::size_t i = 0; i < levels.size() && i < wanted.size(); ++i) {
            wanted[wanted.size() - levels.size() + i] = levels[i];
        }
    }
    if (now == Mode::kHidden || !hwnd) return;
    // Bars rise at once and fall back slowly.
    for (int i = 0; i < kBars; ++i) {
        shown_levels[i] = (std::max)(wanted[i], shown_levels[i] * 0.82f);
    }

    // Where: the bottom of the work area of the screen the focused program is on.
    HWND focus = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(focus ? focus : hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
    scale = dpi_x / 96.0f;

    const Strings& strings = in_english ? kEnglish : kChinese;
    IDWriteTextFormat* format = nullptr;
    dwrite->CreateTextFormat(colors.font_name.empty() ? L"Microsoft YaHei UI" : colors.font_name.c_str(),
                             nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, kTextSize, in_english ? L"en-us" : L"zh-cn",
                             &format);
    if (!format) return;
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    std::wstring text;
    float bar_width = kBarWidth;
    if (now == Mode::kMessage) {
        text = strings.messages[static_cast<int>(shown_message)];
        bar_width = (std::min)(560.0f, (std::max)(200.0f, kSideGap + kButton + 12.0f +
                                                              text_width(text, format) + 22.0f));
    }
    const float width_dip = bar_width + 2 * kShadow;
    const float height_dip = kBarHeight + 2 * kShadow;
    const SIZE size = {static_cast<LONG>(std::ceil(width_dip * scale)),
                       static_cast<LONG>(std::ceil(height_dip * scale))};
    const RECT& work = info.rcWork;
    const POINT position = {
        work.left + (work.right - work.left - size.cx) / 2,
        work.bottom - size.cy - static_cast<LONG>(std::lround((kBottomGap - kShadow) * scale))};

    if (!memory_dc || bitmap_size.cx != size.cx || bitmap_size.cy != size.cy) {
        free_bitmap();
        HDC screen = GetDC(nullptr);
        memory_dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = size.cx;
        bmi.bmiHeader.biHeight = -size.cy;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            free_bitmap();
            format->Release();
            return;
        }
        old_bitmap = SelectObject(memory_dc, bitmap);
        bitmap_size = size;
    }
    if (!target) {
        // Drawn in software: a layered window's pixels, and no graphics card to depend on
        // (some cards drew nothing into windows on another card's screen).
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        factory->CreateDCRenderTarget(&props, &target);
        if (!target) {
            format->Release();
            return;
        }
    }
    const RECT bounds = {0, 0, size.cx, size.cy};
    target->BindDC(memory_dc, &bounds);
    target->SetDpi(96.0f * scale, 96.0f * scale);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    ID2D1SolidColorBrush* brush = nullptr;
    target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.035f), &brush);
    if (brush) {
        const float radius = kBarHeight / 2;
        // Shadow: a few widening rounded rectangles, faint.
        for (int i = 6; i >= 1; --i) {
            const float grow = static_cast<float>(i) * 1.4f;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(kShadow - grow, kShadow - grow + 2.0f,
                                              kShadow + bar_width + grow, kShadow + kBarHeight + grow + 2.0f),
                                  radius + grow, radius + grow),
                brush);
        }
        const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
            D2D1::RectF(kShadow, kShadow, kShadow + bar_width, kShadow + kBarHeight), radius, radius);
        brush->SetColor(color(colors.background, 0.98f));
        target->FillRoundedRectangle(pill, brush);
        brush->SetColor(color(colors.border));
        target->DrawRoundedRectangle(pill, brush, 1.0f);

        const float middle = kShadow + kBarHeight / 2;
        const D2D1_POINT_2F left = D2D1::Point2F(kShadow + kSideGap + kButton / 2, middle);
        stop_center = D2D1::Point2F(kShadow + bar_width - kSideGap - kButton / 2, middle);
        stop_visible = now == Mode::kListening;
        const cxxime::Color accent = colors.hilited_back;
        const cxxime::Color on_accent = colors.hilited_text;
        if (now == Mode::kListening) {
            draw_microphone(left, color(accent), color(on_accent));
            // The waveform: newest on the right; a dot where nobody spoke.
            const float from = left.x + kButton / 2 + 14.0f;
            const float to = stop_center.x - kButton / 2 - 14.0f;
            const float step = (to - from) / kBars;
            brush->SetColor(color(accent, 0.9f));
            for (int i = 0; i < kBars; ++i) {
                const float x = from + step * (i + 0.5f);
                const float h = 3.0f + shown_levels[i] * (kBarHeight - 22.0f);
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x - 1.5f, middle - h / 2, x + 1.5f, middle + h / 2),
                                      1.5f, 1.5f),
                    brush);
            }
            // Stop: a square in a circle.
            brush->SetColor(color(mix(colors.background, colors.text, pressed ? 0.24f : hover ? 0.16f : 0.08f)));
            target->FillEllipse(D2D1::Ellipse(stop_center, kButton / 2, kButton / 2), brush);
            brush->SetColor(color(colors.text));
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(stop_center.x - 5.5f, stop_center.y - 5.5f,
                                              stop_center.x + 5.5f, stop_center.y + 5.5f),
                                  2.0f, 2.0f),
                brush);
        } else if (now == Mode::kRecognizing) {
            // The microphone breathes; "Recognizing" with three dots going up and down.
            const float pulse = 0.65f + 0.35f * std::sin(phase * 6.0f);
            draw_microphone(left, color(accent, pulse), color(on_accent));
            const float from = left.x + kButton / 2 + 14.0f;
            brush->SetColor(color(colors.text));
            const std::wstring label = strings.recognizing;
            const float label_width = text_width(label, format);
            target->DrawText(label.c_str(), static_cast<UINT32>(label.size()), format,
                             D2D1::RectF(from, kShadow, from + label_width + 2, kShadow + kBarHeight), brush);
            brush->SetColor(color(colors.comment_text));
            for (int i = 0; i < 3; ++i) {
                const float lift = 3.0f * std::sin(phase * 7.0f - i * 0.9f);
                target->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(from + label_width + 9.0f + i * 8.0f, middle + 1.0f - (std::max)(0.0f, lift)),
                                  2.2f, 2.2f),
                    brush);
            }
        } else {
            draw_microphone(left, color(mix(colors.background, colors.text, 0.10f)), color(colors.comment_text));
            const float from = left.x + kButton / 2 + 12.0f;
            brush->SetColor(color(colors.text));
            target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                             D2D1::RectF(from, kShadow, kShadow + bar_width - 16.0f, kShadow + kBarHeight),
                             brush);
        }
        brush->Release();
    }
    const HRESULT drawn = target->EndDraw();
    format->Release();
    if (drawn == D2DERR_RECREATE_TARGET) release(target);
    if (FAILED(drawn)) return;

    HDC screen = GetDC(nullptr);
    POINT source = {0, 0};
    SIZE window_size = size;
    POINT window_position = position;
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &window_position, &window_size, memory_dc, &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}
