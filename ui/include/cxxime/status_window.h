// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_STATUS_WINDOW_H_
#define CXXIME_STATUS_WINDOW_H_

#include <cstdint>
#include <functional>
#include <vector>

#include <windows.h>

#include <cxxime/ime_menu.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/render_context.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace cxxime {

enum class StatusButton {
    CHINESE_MODE,   // 中/EN
    FULL_SHAPE,     // 全/半
    CHINESE_PUNCT,  // 。/.
    SETTINGS,       // ⚙
};

struct ButtonState {
    bool chinese_mode = true;
    bool caps_lock = false;
    bool full_shape = false;
    bool chinese_punct = true;
    bool english_words = true;  // English mode: word completion ("英") or letters ("a")
    bool pinyin_initials = false;  // Pinyin style: full pinyin ("拼") or initials ("首")
    InputMode input_mode = InputMode::PINYIN;
};

using StatusButtonClickCallback = std::function<void(StatusButton)>;
using StatusGeometryChangeCallback = std::function<void()>;
using StatusPositionChangeCallback = std::function<void(int x, int y)>;
using StatusMenuCommandCallback = std::function<void(ImeMenuCommand)>;

struct ButtonDrawInfo;

class StatusWindow {
public:
    StatusWindow();
    ~StatusWindow();

    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    // Called from DllMain(DLL_PROCESS_DETACH) — destroy all lingering windows.
    static void cleanup_all();

    bool create(const StatusTheme& theme);
    void destroy();
    bool is_created() const;

    void show();
    void show_preserving_z_order();
    // Only moves this window; upper_window may belong to another process.
    void show_below(HWND upper_window);
    void hide();
    bool is_visible() const;
    bool get_window_rect(RECT* rect) const;
    UINT dpi() const;

    void set_enabled(bool enabled);

    void update_state(const ButtonState& state);
    void set_theme(const StatusTheme& theme);
    void set_position(int x, int y);
    void get_position(int& x, int& y) const;
    void set_click_callback(StatusButtonClickCallback callback);
    void set_geometry_changed_callback(StatusGeometryChangeCallback callback);
    void set_position_callback(StatusPositionChangeCallback callback);
    void set_menu_command_callback(StatusMenuCommandCallback callback);

    // Test accessor
    HWND hwnd_for_test() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool RedrawLayered();
    void UpdateDpiResources(UINT dpi);
    bool PlaceWindow(int x, int y, HMONITOR target_monitor, bool dragging);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnRButtonUp(int x, int y);

    // Rendering: D2D primary, GDI+ fallback — both render to layered_dc_
    void InitLayeredSurface();
    void CleanupLayeredSurface();
    void InitD2D();
    void CleanupD2D();
    void ComputeButtonDrawInfo(std::vector<ButtonDrawInfo>& out);
    void PaintD2D();
    void PaintGdiplus();

    // Coordinate helpers
    RECT GetInputModeRect() const;
    RECT GetSeparatorRect() const;
    RECT GetPillButtonRect(int index) const;
    int HitTest(int x, int y) const;

    void CreateFonts();
    void InitTooltip();
    void ShowContextMenu(int x, int y);

    // Drag: DPI-aware threshold to distinguish click from drag
    int drag_threshold() const { return Scaled(6); }
    void ContinueTracking(int x, int y);

    // Layout constants (base values; actual rendering multiplied by dpi_scale_)
    static constexpr int BUTTON_COUNT = 4;            // Interactive buttons (excludes input mode)
    static constexpr int BASE_BUTTON_WIDTH = 28;      // Function button width
    static constexpr int BASE_SETTINGS_WIDTH = 24;    // Settings button width
    static constexpr int BASE_BUTTON_HEIGHT = 22;     // Button height
    static constexpr int BASE_BUTTON_GAP = 4;         // Gap between buttons
    static constexpr int BASE_SEPARATOR_GAP = 8;      // Gap on each side of separator
    static constexpr int BASE_SEPARATOR_WIDTH = 1;    // Separator line width
    static constexpr int BASE_WINDOW_PADDING = 6;     // Inner padding
    static constexpr int BASE_INPUT_MODE_WIDTH = 28;  // Non-interactive input mode width

    // DPI scaling
    float dpi_scale_ = 1.0f;
    int Scaled(int base) const { return static_cast<int>(base * dpi_scale_ + 0.5f); }
    int WindowWidth() const {
        // Use GetPillButtonRect accumulation to match actual button positions
        RECT last = GetPillButtonRect(BUTTON_COUNT - 1);
        return last.right + Scaled(BASE_WINDOW_PADDING);
    }
    int WindowHeight() const {
        return Scaled(BASE_BUTTON_HEIGHT + 2 * BASE_WINDOW_PADDING);
    }

    // ── Window ───────────────────────────────────────────────
    HWND hwnd_ = nullptr;
    HWND tooltip_hwnd_ = nullptr;
    int win_w_ = 0, win_h_ = 0;

    // ── Layered window: offscreen 32-bit ARGB surface ────────
    HBITMAP layered_bmp_ = nullptr;
    HDC layered_dc_ = nullptr;
    void* layered_bits_ = nullptr;

    // ── D2D (primary, renders to layered DC) ──────────────────
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* d2d_rt_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* d2d_font_cn_ = nullptr;
    IDWriteTextFormat* d2d_font_en_ = nullptr;
    IDWriteTextFormat* d2d_font_icon_ = nullptr;
    bool use_d2d_ = false;
    bool gdiplus_initialized_ = false;

    // ── GDI+ fallback fonts ───────────────────────────────────
    HFONT font_cn_ = nullptr;
    HFONT font_en_ = nullptr;
    HFONT font_icon_ = nullptr;

    // ── State ─────────────────────────────────────────────────
    StatusTheme theme_;
    ButtonState state_;
    int hovered_button_ = -1;
    bool is_enabled_ = true;
    bool layered_ready_ = false;
    bool placement_in_progress_ = false;
    UINT pending_dpi_ = 0;

    // ── Drag state ────────────────────────────────────────────
    bool is_tracking_ = false;
    bool is_dragging_ = false;
    POINT track_start_ = {};
    POINT window_start_ = {};
    POINT drag_requested_ = {};
    HMONITOR drag_monitor_ = nullptr;
    bool fit_drag_to_monitor_ = false;

    StatusButtonClickCallback click_callback_;
    StatusGeometryChangeCallback geometry_changed_callback_;
    StatusPositionChangeCallback position_callback_;
    StatusMenuCommandCallback menu_command_callback_;
};

} // namespace cxxime

#endif // CXXIME_STATUS_WINDOW_H_
