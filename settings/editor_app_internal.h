// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SETTINGS_EDITOR_APP_INTERNAL_H_
#define CXXIME_SETTINGS_EDITOR_APP_INTERNAL_H_

#include <initializer_list>
#include <string>

#include <windows.h>
#include <commctrl.h>

namespace cxxime {
namespace settings {

inline constexpr int kFontPt = 14;
inline constexpr int kNavFontPt = kFontPt + 1;
inline constexpr int kLearningPanel = 4;  // page index of Learning
inline constexpr int kUpdatePanel = 6;    // page index of Updates

extern float g_dpi;
extern HFONT g_hFont;
extern int kListW;
extern int kPadX;
extern int kPadY;
extern int kCtrlH;
extern int kRowH;
extern int kPanelPadTop;
extern int kPanelPadLeft;
extern int kLblW;
extern int kCtlX;

int S(int value);

// Settings window colors, light or dark (editor_theme.cc; EditorApp::apply_ui_theme). Pages are
// cards on the window background: controls on a page take the card color.
struct UiColors {
    bool dark;
    COLORREF window;        // behind the cards, and the page list
    COLORREF text;
    COLORREF hint;
    COLORREF control;       // edit and list boxes
    COLORREF link;
    COLORREF accent;        // primary button, progress, focus
    COLORREF card;
    COLORREF border;        // card and button outlines
    COLORREF nav_selected;  // the selected page in the list
    COLORREF nav_selected_text;
    COLORREF button_hover;
};
const UiColors& ui_colors();
void set_ui_dark(bool dark);
HBRUSH window_brush();
HBRUSH card_brush();
HBRUSH control_brush();
void init_layout();
HFONT get_font();
HFONT get_title_font();  // card titles
HFONT get_small_font();  // secondary text drawn by hand
HFONT get_icon_font(int point);  // Segoe Fluent Icons, else Segoe MDL2 Assets
void release_shared_fonts();     // the fonts above

// Cards: a page is a column of rounded cards, each with an optional title. Controls go inside
// at x >= kPanelPadLeft; card_begin() returns the y of the first row, card_end() the top of the
// next card. The page window paints the cards (paint_panel, from PanelForwardProc).
int card_begin(HWND panel, int top, const wchar_t* title);
int card_end(HWND panel, int content_bottom);
void clear_cards();
// A page taller than the window scrolls: call before its controls are made (the bar takes
// its width), then update_panel_scroll() once they are.
void make_panel_scrollable(HWND panel);
void update_panel_scroll(HWND panel);
void set_card_visible(HWND panel, int index, bool visible);  // index in card_begin order
void paint_panel(HWND panel, HDC dc);
void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border);

// Push buttons are drawn by hand (BS_OWNERDRAW, draw_button from WM_DRAWITEM): rounded, with a
// hover color; a primary button is filled with the accent color.
void set_button_primary(HWND button, bool primary);
void set_button_selected(HWND button, bool selected);  // a tab: tinted like the selected page
void draw_button(const DRAWITEMSTRUCT& item);
void enable_button_hover(HWND button);
HWND make_page_button(int id, const wchar_t* text, int x, int y, int width, int height,
                      HWND parent);

// Width of a right-aligned label column: the widest of these labels (lengths differ between
// languages).
int label_width(std::initializer_list<const char*> keys);
int make_label(const wchar_t* text, int x, int y, HWND parent);
void make_aligned_label(const wchar_t* text, int y, HWND parent);
int make_aligned_label(const wchar_t* text, int x, int width, int y, HWND parent);
HWND make_edit(int id, int x, int y, int width, HWND parent);
HWND make_combo(int id, int x, int y, int width, HWND parent);
// The mouse wheel over a closed drop-down list scrolls the page (it would otherwise change the
// selection, and on the General page start a graphics card test). make_combo does this already.
void scroll_page_on_wheel(HWND combo);
void set_combo_drop_count(HWND combo, int count);
HWND make_check(int id, const wchar_t* text, int x, int y, int width, HWND parent);
// A web link (opened in the browser; right-click copies it). Its address comes from
// web_link_url(id).
constexpr int kPrivacyDocLinkId = 5004;  // docs/privacy*.md on GitHub (ui string privacy.doc_url)
constexpr int kReleaseLinkId = 5005;     // the latest release on GitHub (update::kReleasesPage)
// The data location row on the General page (editor_data_folder.cc).
constexpr int kDataChangeId = 5010;
constexpr int kDataDefaultId = 5011;
// Offline translation on the Learning page (editor_translator.cc).
constexpr int kTranslatorId = 5020;
constexpr int kTranslatorDeviceId = 5021;
constexpr int kTranslatorActionId = 5022;
constexpr int kTranslatorRemoveId = 5023;
HWND make_web_link(int id, const wchar_t* text, int x, int y, int width, HWND parent);
HWND make_button(int id, const wchar_t* text, int x, int y, int width, HWND parent);
HWND make_radio(int id, const wchar_t* text, int x, int y, int width, HWND parent, bool group);
void combo_add(HWND combo, const wchar_t* text);
void combo_sel(HWND combo, const wchar_t* text);
int combo_index(HWND combo);
void combo_set_index(HWND combo, int index);

std::wstring utf8_to_wstr(const std::string& text);
std::string wstr_to_utf8(const std::wstring& text);
std::string edit_text_utf8(HWND edit);
std::wstring path_for_display(const std::string& path);
bool copy_text_to_clipboard(HWND owner, const wchar_t* text);

void set_edit_int(HWND edit, int value);
int get_edit_int(HWND edit);
bool get_check(HWND control);
void set_check(HWND control, bool checked);

LRESULT CALLBACK PanelForwardProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR subclass_id, DWORD_PTR reference_data);

} // namespace settings
} // namespace cxxime

#endif // CXXIME_SETTINGS_EDITOR_APP_INTERNAL_H_
