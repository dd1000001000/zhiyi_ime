// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Win32 native controls settings editor.
//
// Modified by Zhiyi IME Contributors: reduced to four pages (General, Keys, Dictionary, About)
// with translated strings (i18n.h).

#ifndef CXXIME_SETTINGS_EDITOR_APP_H_
#define CXXIME_SETTINGS_EDITOR_APP_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>

#include <cxxime/config.h>
#include <cxxime/settings_route.h>
#include <cxxime/update.h>

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
    void create_privacy_panel(HWND panel);
    void open_log_folder();
    void delete_experience_log();
    void on_privacy_check(int control_id);
    void create_update_panel(HWND panel);
    void create_about_panel(HWND panel, int panel_width);
    void show_panel(int idx);
    void release_fonts();

    // Config <-> controls
    bool load_config();
    bool read_config_files(cxxime::Config& config, bool report_errors);
    // Picks up changes made elsewhere since the last load (taskbar menu, another settings window)
    // while keeping the unsaved choices on the pages.
    void refresh_config();
    void populate_controls();
    // False when a value is invalid (with a message box if report_errors).
    bool read_controls(bool report_errors = true);
    bool save_config();

    bool handle_command(int control_id, int notification);
    bool handle_about_notify(LPARAM notification);
    // Light or dark window (editor_theme.cc), following the IME theme.
    void apply_ui_theme(bool dark, bool force = false);
    LRESULT control_colors(UINT message, HDC dc, HWND control);
    bool draw_choice_button(LPARAM notification, LRESULT* result);
    // Updates (editor_update_panel.cc)
    void init_update();
    void show_update_state();
    void start_update_check(bool automatic);
    void on_update_checked(bool automatic, const update::CheckResult& result);
    void show_update_prompt();
    void start_update_download();
    void on_update_downloaded(const std::wstring& path, update::Status status);
    bool handle_update_command(int control_id, int notification);
    bool handle_update_message(UINT message, WPARAM wparam, LPARAM lparam);
    void update_enabled_controls();
    void clear_learning_data();
    void set_switch_key_boxes(const cxxime::Config& config);
    void update_switch_key_notes();
    void restore_default_keys();

    HWND hwnd_ = nullptr;
    HWND hList_ = nullptr;
    HWND hFooter_ = nullptr;
    HWND hAboutTitle_ = nullptr;
    HFONT hFooterFont_ = nullptr;
    HFONT hListFont_ = nullptr;
    HFONT hAboutTitleFont_ = nullptr;
    HFONT hHintFont_ = nullptr;
    std::vector<HWND> hints_;  // secondary text, drawn smaller and gray
    HWND make_hint(const wchar_t* text, int x, int y, int width, HWND parent, int lines = 2);
    int panel_ = 0;
    cxxime::SettingsPanel initial_panel_ = cxxime::SettingsPanel::kInput;
    static constexpr int kPanelCount = 7;
    HWND hPanels_[kPanelCount] = {};

    // General
    HWND hPinyin_ = nullptr, hWubi_ = nullptr;
    HWND hFullPinyin_ = nullptr, hInitials_ = nullptr;
    HWND hLight_ = nullptr, hDark_ = nullptr;
    HWND hFontSmall_ = nullptr, hFontMedium_ = nullptr, hFontLarge_ = nullptr;
    HWND hPageSize_ = nullptr;
    HWND hLanguage_ = nullptr;
    HWND hEnglishCorrection_ = nullptr;
    std::vector<UiLanguage> languages_;

    // Fuzzy pinyin: master switch and one check box per pair (FuzzyGroup bit order)
    HWND hFuzzyEnabled_ = nullptr;
    HWND hFuzzyGroups_[7] = {};

    // Keys
    HWND hSwitchKey_ = nullptr;  // key capture boxes (key_capture.h)
    HWND hStyleKey_ = nullptr;
    HWND hPunctKey_ = nullptr;
    HWND hShapeKey_ = nullptr;

    // Dictionary
    HWND hLearning_ = nullptr;

    // Privacy
    HWND hExperience_ = nullptr;
    HWND hCollectInput_ = nullptr;

    // Updates
    HWND hUpdateNotify_ = nullptr;
    HWND hCheckUpdate_ = nullptr;
    HWND hUpdateStatus_ = nullptr;
    HWND hNewVersion_ = nullptr;
    HWND hReleaseLink_ = nullptr;
    HWND hReleaseNotes_ = nullptr;
    HWND hInstallUpdate_ = nullptr;
    HWND hInstallHint_ = nullptr;
    HWND hUpdateProgress_ = nullptr;
    enum class UpdateBusy { kNone, kChecking, kDownloading };
    UpdateBusy update_busy_ = UpdateBusy::kNone;
    std::wstring update_status_;
    bool have_update_ = false;
    bool update_prompted_ = false;  // the reminder is shown once per start
    update::Manifest update_manifest_;
    std::uint64_t update_done_ = 0;
    std::uint64_t update_total_ = 0;
    std::shared_ptr<std::atomic<bool>> update_cancel_;

    cxxime::Config config_;
    cxxime::Config loaded_config_;  // the files as last read or written
    std::string ui_language_;  // resolved code of the strings in use
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
};

} // namespace settings
} // namespace cxxime
#endif
