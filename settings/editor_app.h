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
#include <cxxime/glossary.h>
#include <cxxime/gpu_adapters.h>
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
    void create_learning_panel(HWND panel);
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
    bool on_page(HWND control) const;  // on a settings page (a card), not the window itself
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
    // Backup and restore (editor_backup.cc).
    bool ensure_server_running();  // starts zhiyi-server when needed; false: reported
    // before_import: the backup offered before an import (no questions about unsaved edits
    // or opening the folder). Returns whether a backup was written.
    bool export_user_backup(bool before_import = false);
    void import_user_backup();
    // Learning (editor_learning_panel.cc)
    struct PackState;
    void init_learning();
    void populate_learning();
    void read_learning(cxxime::Config& config);
    void fill_target_combo(HWND combo, const std::string& source, const std::string& selected);
    std::string combo_target(HWND combo, const std::string& source) const;
    void refresh_pack(PackState& pack);
    void show_pack_tab(int tab);
    void layout_pack_rows();
    void paint_pack_list(HWND list, HDC dc);
    void draw_target_item(const DRAWITEMSTRUCT& item);
    void draw_gloss_preview(const DRAWITEMSTRUCT& item);
    void start_pack_check(int pack_index);  // -1: every installed pack
    void on_packs_checked(const update::GlossaryCheckResult& result, int pack_index);
    void start_pack_download(int pack_index);
    void on_pack_downloaded(int pack_index, update::Status status, const std::wstring& path);
    void remove_pack_at(int pack_index);
    void on_pack_action(int pack_index);
    bool handle_learning_command(int control_id, int notification);
    bool handle_learning_message(UINT message, WPARAM wparam, LPARAM lparam);
    void load_gpu_choices();
    void populate_device();
    void fill_device_combo();
    void show_device_result(double gpu_ms, double cpu_ms);
    void show_device_hint(const wchar_t* text);
    std::string selected_device() const;
    void on_device_selected();
    bool handle_gpu_message(UINT message, WPARAM wparam, LPARAM lparam);
    // Data location (editor_data_folder.cc)
    void create_data_card(HWND panel, int y);
    void show_data_folder();
    void choose_data_folder(bool to_default);
    bool handle_data_message(UINT message, WPARAM wparam, LPARAM lparam);
    // Offline translation (editor_translator.cc)
    void create_translator_card(HWND panel, int y);
    void populate_translator();
    void read_translator(cxxime::Config& config);
    void show_translator_state();
    void start_translator_check();
    void start_translator_install();
    void start_translator_test();
    void remove_translator();
    bool handle_translator_command(int control_id, int notification);
    bool handle_translator_message(UINT message, WPARAM wparam, LPARAM lparam);
    bool handle_learning_item(UINT message, LPARAM lparam);  // WM_MEASUREITEM / WM_DRAWITEM
    static LRESULT CALLBACK pack_list_proc(HWND, UINT, WPARAM, LPARAM);
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
    static constexpr int kPanelCount = 8;
    HWND hPanels_[kPanelCount] = {};

    // General
    HWND hPinyin_ = nullptr, hWubi_ = nullptr;
    HWND hFullPinyin_ = nullptr, hInitials_ = nullptr;
    HWND hLight_ = nullptr, hDark_ = nullptr;
    HWND hFontSmall_ = nullptr, hFontMedium_ = nullptr, hFontLarge_ = nullptr;
    HWND hVertical_ = nullptr, hHorizontal_ = nullptr;  // candidate layout
    HWND hPageSize_ = nullptr;
    HWND hLanguage_ = nullptr;
    HWND hEnglishCorrection_ = nullptr;
    HWND hAutostart_ = nullptr;  // startup.autostart
    HWND hLaya_ = nullptr;  // laya.enable (Chinese and English)
    // laya.device (editor_gpu.cc): only created when this computer has a card to offer
    HWND hDevice_ = nullptr;
    HWND hDeviceHint_ = nullptr;
    std::vector<GpuAdapter> gpu_choices_;  // the combo's items after "CPU"
    bool gpu_testing_ = false;
    bool device_warning_ = false;  // the hint says the card is slower than the CPU
    std::vector<UiLanguage> languages_;
    HWND hDataFolder_ = nullptr, hDataHint_ = nullptr;
    HWND hDataChange_ = nullptr, hDataDefault_ = nullptr;
    bool data_moving_ = false;  // a move runs in the background

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

    // Learning
    struct PackState {
        std::string source;
        std::string target;
        bool installed = false;
        bool builtin = false;  // the installer has it (restored without the network)
        std::uint32_t version = 0;
        std::uint32_t entries = 0;
        std::uint64_t size = 0;
        enum class Busy { kNone, kChecking, kDownloading } busy = Busy::kNone;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        bool remote_known = false;  // the server's version below is known
        update::GlossaryPack remote;
        std::wstring note;          // last result: up to date, update available, failure
        bool note_good = false;     // shown in the accent color
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    HWND hChineseTarget_ = nullptr, hEnglishTarget_ = nullptr;
    HWND hGlossPreview_ = nullptr;
    HWND hPackTabs_[2] = {};
    HWND hCheckAllPacks_ = nullptr;
    HWND hPackList_ = nullptr;
    std::vector<HWND> hPackActions_;  // one per row: download / cancel / check / update
    std::vector<HWND> hPackRemoves_;
    std::vector<PackState> packs_;    // zh -> 7 targets, then en -> 7 targets
    int pack_tab_ = 0;
    int last_chinese_target_ = 0, last_english_target_ = 0;  // combo indexes
    HWND hTranslator_ = nullptr, hTranslatorDevice_ = nullptr, hTranslatorStatus_ = nullptr;
    HWND hTranslatorAction_ = nullptr, hTranslatorRemove_ = nullptr, hTranslatorHint_ = nullptr;
    std::vector<GpuAdapter> mt_choices_;  // cards with more than 2 GB of their own memory
    enum class MtBusy { kNone, kChecking, kDownloading, kTesting } mt_busy_ = MtBusy::kNone;
    bool mt_remote_known_ = false;
    update::TranslatorManifest mt_remote_;
    std::uint64_t mt_done_mb_ = 0, mt_total_mb_ = 0;
    std::shared_ptr<std::atomic<bool>> mt_cancel_;
    std::wstring mt_note_;  // the last result (failed, cancelled)

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
