// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Win32 native controls settings editor.
//
// Modified by Zhiyi IME Contributors: reduced to four pages (General, Keys, Dictionary, About)
// with translated strings (i18n.h).

#ifndef CXXIME_SETTINGS_EDITOR_APP_H_
#define CXXIME_SETTINGS_EDITOR_APP_H_

#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>

#include <cxxime/config.h>
#include <cxxime/settings_route.h>

#include "i18n.h"

namespace cxxime {
namespace settings {

class EditorApp {
public:
    static int run(HINSTANCE hInst,
                   float dpiScale = 1.0f,
                   cxxime::SettingsPanel initialPanel = cxxime::SettingsPanel::kInput);

private:
    // Window and pages
    void create_controls(HWND hwnd);
    void destroy_controls();
    void rebuild_ui();  // after the UI language changed
    void create_general_panel(HWND panel);
    void create_fuzzy_panel(HWND panel);
    void create_keys_panel(HWND panel);
    void create_dictionary_panel(HWND panel);
    void create_about_panel(HWND panel, int panel_width);
    void show_panel(int idx);
    void release_fonts();

    // Config <-> controls
    bool load_config();
    void populate_controls();
    // False when a value is invalid (with a message box if report_errors).
    bool read_controls(bool report_errors = true);
    bool save_config();

    bool handle_command(int control_id, int notification);
    bool handle_about_notify(LPARAM notification);
    void update_enabled_controls();
    void clear_learning_data();

    HWND hwnd_ = nullptr;
    HWND hList_ = nullptr;
    HWND hFooter_ = nullptr;
    HWND hAboutTitle_ = nullptr;
    HFONT hFooterFont_ = nullptr;
    HFONT hListFont_ = nullptr;
    HFONT hAboutTitleFont_ = nullptr;
    HFONT hHintFont_ = nullptr;
    std::vector<HWND> hints_;  // secondary text, drawn smaller and gray
    HWND make_hint(const wchar_t* text, int x, int y, int width, HWND parent);
    int panel_ = 0;
    cxxime::SettingsPanel initial_panel_ = cxxime::SettingsPanel::kInput;
    static constexpr int kPanelCount = 5;
    HWND hPanels_[kPanelCount] = {};

    // General
    HWND hPinyin_ = nullptr, hWubi_ = nullptr;
    HWND hFullPinyin_ = nullptr, hInitials_ = nullptr;
    HWND hLight_ = nullptr, hDark_ = nullptr;
    HWND hFontSmall_ = nullptr, hFontMedium_ = nullptr, hFontLarge_ = nullptr;
    HWND hPageSize_ = nullptr;
    HWND hLanguage_ = nullptr;
    std::vector<UiLanguage> languages_;

    // Fuzzy pinyin: master switch and one check box per pair (FuzzyGroup bit order)
    HWND hFuzzyEnabled_ = nullptr;
    HWND hFuzzyGroups_[7] = {};

    // Keys
    HWND hSwitchKey_ = nullptr;
    HWND hStyleEnabled_ = nullptr;
    HWND hStyleKey_ = nullptr;

    // Dictionary
    HWND hLearning_ = nullptr;

    cxxime::Config config_;
    std::string ui_language_;  // resolved code of the strings in use
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
};

} // namespace settings
} // namespace cxxime
#endif
