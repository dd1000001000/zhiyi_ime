// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SETTINGS_EDITOR_APP_INTERNAL_H_
#define CXXIME_SETTINGS_EDITOR_APP_INTERNAL_H_

#include <string>

#include <windows.h>
#include <commctrl.h>

namespace cxxime {
namespace settings {

inline constexpr int kFontPt = 14;
inline constexpr int kNavFontPt = kFontPt + 1;
inline constexpr int kUpdatePanel = 5;  // page index of Updates

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

// Settings window colors, light or dark (editor_theme.cc; EditorApp::apply_ui_theme).
struct UiColors {
    bool dark;
    COLORREF window;
    COLORREF text;
    COLORREF hint;
    COLORREF control;   // edit and list boxes
    COLORREF link;
    COLORREF selected;  // the selected page in the list
};
const UiColors& ui_colors();
void set_ui_dark(bool dark);
HBRUSH window_brush();
HBRUSH control_brush();
void init_layout();
HFONT get_font();

int make_label(const wchar_t* text, int x, int y, HWND parent);
void make_aligned_label(const wchar_t* text, int y, HWND parent);
int make_aligned_label(const wchar_t* text, int x, int width, int y, HWND parent);
HWND make_edit(int id, int x, int y, int width, HWND parent);
HWND make_combo(int id, int x, int y, int width, HWND parent);
void set_combo_drop_count(HWND combo, int count);
HWND make_check(int id, const wchar_t* text, int x, int y, int width, HWND parent);
// A web link (opened in the browser; right-click copies it). Its address comes from
// web_link_url(id).
constexpr int kPrivacyDocLinkId = 5004;  // docs/privacy*.md on GitHub (ui string privacy.doc_url)
constexpr int kReleaseLinkId = 5005;     // the latest release on GitHub (update::kReleasesPage)
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
