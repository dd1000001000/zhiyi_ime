// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Settings pages: General (Chinese input, pinyin style, theme, font size, candidate count, UI
// language, English spelling correction), Fuzzy pinyin,
// Keys (Chinese/English, style, punctuation and full/half width switch keys), Dictionary
// (self-learning) and Privacy (user experience improvement program).

#include "editor_app.h"

#include <algorithm>
#include <cwchar>
#include <string>

#include <shellapi.h>

#include <cxxime/diagnostic_log_path.h>
#include <cxxime/experience_log.h>
#include <cxxime/keyboard_shortcut.h>
#include <cxxime/lexicon_control.h>
#include <cxxime/user_dict.h>

#include "editor_app_internal.h"
#include "hotkey_check.h"
#include "key_capture.h"

namespace cxxime {
namespace settings {
namespace {

enum ControlId {
    kPinyinId = 1001,
    kWubiId,
    kFullPinyinId,
    kInitialsId,
    kLightId,
    kDarkId,
    kFontSmallId,
    kFontMediumId,
    kFontLargeId,
    kLanguageId,
    kPageSizeId,
    kEnglishCorrectionId,
    kVerticalId,
    kHorizontalId,
    kAutostartId,
    kSwitchKeyId = 1101,
    kStyleKeyId,
    kPunctKeyId,
    kShapeKeyId,
    kRestoreKeysId,
    kLearningId = 1201,
    kClearLearningId,
    kExportBackupId,
    kImportBackupId,
    kExperienceId = 1251,
    kCollectInputId,
    kOpenLogsId,
    kDeleteLogsId,
    kFuzzyEnabledId = 1301,
    kFuzzyGroupFirstId = 1311,  // .. 1317, FuzzyGroup bit order
};

// Fuzzy pinyin pairs in FuzzyGroup bit order (engine/include/cxxime/spellings_index.h):
// four initials (left column) and three finals (right column).
const wchar_t* const kFuzzyGroupLabels[] = {
    L"z = zh", L"c = ch", L"s = sh", L"n = l", L"an = ang", L"en = eng", L"in = ing",
};
constexpr int kFuzzyGroupCount = 7;
constexpr int kFuzzyInitialCount = 4;

constexpr char kLightTheme[] = "moon_light";
constexpr char kDarkTheme[] = "moon_dark";
constexpr int kFontSmall = 12;
constexpr int kFontMedium = 14;
constexpr int kFontLarge = 17;
constexpr int kMinPageSize = 3;
constexpr int kMaxPageSize = 10;


bool switch_action_enabled(const Config& config, const char* key) {
    const auto found = config.ascii_switch_key.find(key);
    return found != config.ascii_switch_key.end() && found->second != "noop";
}

// Chinese/English switch: a combination (shortcuts.ascii_toggle), else Shift or Ctrl tapped
// alone (ascii_composer switch_key, either side), else none.
KeyChoice switch_key_choice(const Config& config) {
    if (config.ascii_toggle_shortcut.enabled()) {
        return KeyChoice::of(config.ascii_toggle_shortcut);
    }
    if (switch_action_enabled(config, "Shift_L") || switch_action_enabled(config, "Shift_R")) {
        return KeyChoice::tap(VK_SHIFT);
    }
    if (switch_action_enabled(config, "Control_L") || switch_action_enabled(config, "Control_R")) {
        return KeyChoice::tap(VK_CONTROL);
    }
    return KeyChoice::none();
}

KeyboardShortcut combo_of(const KeyChoice& choice) {
    return choice.kind == KeyChoice::Kind::kCombo ? choice.combo : KeyboardShortcut{};
}

// "中英切换:" -> "中英切换" for messages.
std::wstring label_name(const char* key) {
    std::wstring name = tr(key);
    while (!name.empty() && (name.back() == L':' || name.back() == L'\uFF1A' ||
                             name.back() == L' ')) {
        name.pop_back();
    }
    return name;
}

void apply_switch_key_choice(Config& config, const KeyChoice& choice) {
    // "code": the keys typed so far are committed as letters, then the mode switches.
    const bool tap = choice.kind == KeyChoice::Kind::kTap;
    const char* shift = tap && choice.tap_key == VK_SHIFT ? "code" : "noop";
    const char* ctrl = tap && choice.tap_key == VK_CONTROL ? "code" : "noop";
    config.ascii_switch_key["Shift_L"] = shift;
    config.ascii_switch_key["Shift_R"] = shift;
    config.ascii_switch_key["Control_L"] = ctrl;
    config.ascii_switch_key["Control_R"] = ctrl;
    config.ascii_toggle_shortcut =
        choice.kind == KeyChoice::Kind::kCombo ? choice.combo : KeyboardShortcut{};
}

} // namespace

HWND EditorApp::make_hint(const wchar_t* text, int x, int y, int width, HWND parent, int lines) {
    RECT panel_rect = {};
    GetClientRect(parent, &panel_rect);
    width = (std::min)(width, static_cast<int>(panel_rect.right) - x - S(8));
    if (!hHintFont_) {
        hHintFont_ = CreateFontW(-S(kFontPt - 2), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    }
    // Two lines by default: long hints wrap.
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y,
                                   width, S(lines * (kFontPt + 6)), parent, nullptr,
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(hHintFont_), TRUE);
    hints_.push_back(control);
    return control;
}

void EditorApp::create_general_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    const int labels = label_width({"general.chinese_input", "general.pinyin_style",
                                    "general.theme", "general.font_size", "general.layout",
                                    "general.page_size", "general.language", "general.english",
                                    "general.startup"});
    int y = card_begin(panel, kPanelPadTop, tr("general.card_input"));
    const int option_width = S(110);
    auto radios = [&](const char* label, std::initializer_list<std::pair<int, const char*>> items,
                      std::initializer_list<HWND*> handles) {
        const int x = make_aligned_label(tr(label), x0, labels, y, panel);
        auto handle = handles.begin();
        int i = 0;
        for (const auto& [id, key] : items) {
            **handle = make_radio(id, tr(key), x + i * option_width, y, option_width - S(6), panel,
                                  i == 0);
            ++handle;
            ++i;
        }
        y += kRowH;
        return x;
    };

    radios("general.chinese_input", {{kPinyinId, "general.pinyin"}, {kWubiId, "general.wubi"}},
           {&hPinyin_, &hWubi_});
    const int x = radios("general.pinyin_style",
                         {{kFullPinyinId, "general.full_pinyin"}, {kInitialsId, "general.initials"}},
                         {&hFullPinyin_, &hInitials_});
    make_hint(tr("general.pinyin_style_hint"), x, y - S(6), S(400), panel);
    y += S(40);
    const int english_x = make_aligned_label(tr("general.english"), x0, labels, y, panel);
    hEnglishCorrection_ = make_check(kEnglishCorrectionId, tr("general.english_correction"),
                                     english_x, y, S(300), panel);
    y += kRowH;
    make_hint(tr("general.english_correction_hint"), english_x, y - S(6), S(480), panel, 1);
    y += S(20);
    const int startup_x = make_aligned_label(tr("general.startup"), x0, labels, y, panel);
    hAutostart_ = make_check(kAutostartId, tr("general.autostart"), startup_x, y, S(300), panel);
    y += kRowH;
    make_hint(tr("general.autostart_hint"), startup_x, y - S(6), S(480), panel, 1);
    y = card_end(panel, y + S(14));

    y = card_begin(panel, y, tr("general.card_appearance"));
    radios("general.theme", {{kLightId, "general.light"}, {kDarkId, "general.dark"}},
           {&hLight_, &hDark_});
    radios("general.font_size",
           {{kFontSmallId, "general.small"}, {kFontMediumId, "general.medium"},
            {kFontLargeId, "general.large"}},
           {&hFontSmall_, &hFontMedium_, &hFontLarge_});
    const int layout_x = radios(
        "general.layout", {{kVerticalId, "general.vertical"}, {kHorizontalId, "general.horizontal"}},
        {&hVertical_, &hHorizontal_});
    make_hint(tr("general.layout_hint"), layout_x, y - S(6), S(400), panel, 1);
    y += S(20);

    const int page_x = make_aligned_label(tr("general.page_size"), x0, labels, y, panel);
    hPageSize_ = make_combo(kPageSizeId, page_x, y, S(80), panel);
    for (int count = kMinPageSize; count <= kMaxPageSize; ++count) {
        combo_add(hPageSize_, std::to_wstring(count).c_str());
    }
    y += kRowH;

    const int combo_x = make_aligned_label(tr("general.language"), x0, labels, y, panel);
    hLanguage_ = make_combo(kLanguageId, combo_x, y, S(220), panel);
    combo_add(hLanguage_, tr("general.follow_system"));
    languages_ = available_ui_languages();
    for (const UiLanguage& language : languages_) {
        combo_add(hLanguage_, language.name.c_str());
    }
    y += kRowH;
    card_end(panel, y - S(6));
}

void EditorApp::create_fuzzy_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = card_begin(panel, kPanelPadTop, tr("nav.fuzzy"));
    hFuzzyEnabled_ = make_check(kFuzzyEnabledId, tr("fuzzy.enable"), x0, y, S(400), panel);
    y += kRowH + S(4);

    const int column_width = S(170);
    const int indent = x0 + S(22);
    make_label(tr("fuzzy.initials"), indent, y, panel);
    make_label(tr("fuzzy.finals"), indent + column_width, y, panel);
    y += kRowH;
    for (int i = 0; i < kFuzzyGroupCount; ++i) {
        const bool initial = i < kFuzzyInitialCount;
        const int row = initial ? i : i - kFuzzyInitialCount;
        hFuzzyGroups_[i] = make_check(kFuzzyGroupFirstId + i, kFuzzyGroupLabels[i],
                                      indent + (initial ? 0 : column_width), y + row * kRowH,
                                      column_width - S(10), panel);
    }
    y += kFuzzyInitialCount * kRowH + S(6);
    make_hint(tr("fuzzy.hint"), x0, y, S(500), panel);
    card_end(panel, y + S(40));
}

void EditorApp::create_keys_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = card_begin(panel, kPanelPadTop, tr("keys.card"));
    const int labels = label_width({"keys.switch", "keys.style", "keys.punct", "keys.shape"});
    const int box_width = S(340);
    struct Row {
        const char* label;
        int id;
        HWND* box;
        const char* hint;
    };
    // Only the Chinese/English switch takes Shift or Ctrl tapped alone.
    const Row rows[] = {
        {"keys.switch", kSwitchKeyId, &hSwitchKey_, "keys.switch_hint"},
        {"keys.style", kStyleKeyId, &hStyleKey_, "keys.style_hint"},
        {"keys.punct", kPunctKeyId, &hPunctKey_, nullptr},
        {"keys.shape", kShapeKeyId, &hShapeKey_, nullptr},
    };
    for (const Row& row : rows) {
        const int x = make_aligned_label(tr(row.label), x0, labels, y, panel);
        *row.box = create_key_capture(row.id, x, y, box_width, kCtrlH, panel,
                                      row.box == &hSwitchKey_);
        y += kRowH;
        if (row.hint) {
            make_hint(tr(row.hint), x, y - S(6), S(380), panel);
            y += kRowH;
        } else {
            y += S(8);
        }
    }
    // A key already used by another box is not taken: the box keeps its previous key.
    for (const Row& row : rows) {
        HWND self = *row.box;
        key_capture_set_check(self, [self, rows](const KeyChoice& choice) {
            for (const Row& other : rows) {
                if (*other.box != self && same_key_choice(choice, key_capture_get(*other.box))) {
                    std::wstring notice = tr("keys.taken");
                    const size_t at = notice.find(L"{0}");
                    if (at != std::wstring::npos) notice.replace(at, 3, label_name(other.label));
                    return notice;
                }
            }
            return std::wstring{};
        });
    }
    make_hint(tr("keys.capture_hint"), x0, y - S(4), S(500), panel);
    y += S(40);
    make_button(kRestoreKeysId, tr("keys.restore"), x0, y, S(160), panel);
    card_end(panel, y + S(30));
}

void EditorApp::create_dictionary_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = card_begin(panel, kPanelPadTop, tr("dictionary.card"));
    hLearning_ = make_check(kLearningId, tr("dictionary.learning"), x0, y, S(400), panel);
    y += kRowH;
    make_hint(tr("dictionary.learning_hint"), x0 + S(20), y - S(6), S(440), panel);
    y += S(40);
    make_button(kClearLearningId, tr("dictionary.clear"), x0, y, S(160), panel);
    y = card_end(panel, y + S(30));

    // editor_backup.cc
    y = card_begin(panel, y, tr("backup.card"));
    make_hint(tr("backup.hint"), x0, y, S(560), panel, 3);
    y += S(3 * (kFontPt + 6) + 8);
    make_button(kExportBackupId, tr("backup.export"), x0, y, S(120), panel);
    make_button(kImportBackupId, tr("backup.import"), x0 + S(130), y, S(120), panel);
    card_end(panel, y + S(30));
}

// Two tiers (cxxime/experience_log.h, docs/privacy.md): input collection needs the
// experience program, so checking it checks the program (after a confirmation) and unchecking
// the program unchecks it. Revoking keeps the logs; "Delete all records" removes both tiers'.
void EditorApp::create_privacy_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = card_begin(panel, kPanelPadTop, tr("privacy.card_program"));
    hExperience_ = make_check(kExperienceId, tr("privacy.experience"), x0, y, S(480), panel);
    y += kRowH;
    make_hint(tr("privacy.what"), x0 + S(20), y - S(6), S(480), panel);
    y += S(40);
    hCollectInput_ =
        make_check(kCollectInputId, tr("privacy.collect_input"), x0, y, S(480), panel);
    y += kRowH;
    make_hint(tr("privacy.input_what"), x0 + S(20), y - S(6), S(480), panel);
    y += S(34);
    y = card_end(panel, y);

    y = card_begin(panel, y, tr("privacy.card_records"));
    make_hint(tr("privacy.local"), x0, y, S(500), panel, 1);
    y += S(kFontPt + 12);
    // The full list of what each tier records (docs/privacy*.md on GitHub).
    make_web_link(kPrivacyDocLinkId, tr("privacy.doc_link"), x0, y, S(300), panel);
    y += kRowH + S(4);
    make_button(kOpenLogsId, tr("privacy.open_logs"), x0, y, S(160), panel);
    make_button(kDeleteLogsId, tr("privacy.delete_logs"), x0 + S(172), y, S(160), panel);
    card_end(panel, y + S(30));
}

void EditorApp::on_privacy_check(int control_id) {
    if (control_id == kCollectInputId && get_check(hCollectInput_)) {
        if (MessageBoxW(hwnd_, tr("privacy.input_confirm"), tr("window.title"),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            set_check(hExperience_, true);
        } else {
            set_check(hCollectInput_, false);
        }
    } else if (control_id == kExperienceId && !get_check(hExperience_)) {
        set_check(hCollectInput_, false);
    }
}

void EditorApp::open_log_folder() {
    const std::wstring directory = diagnostic_log_directory();  // created when missing
    if (!directory.empty()) {
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void EditorApp::delete_experience_log() {
    if (MessageBoxW(hwnd_, tr("privacy.delete_confirm"), tr("window.title"),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    ExperienceLog::delete_all_logs();
    MessageBoxW(hwnd_, tr("privacy.deleted"), tr("window.title"), MB_OK | MB_ICONINFORMATION);
}

void EditorApp::populate_controls() {
    set_check(hPinyin_, config_.input_mode != 1);
    set_check(hWubi_, config_.input_mode == 1);
    set_check(hFullPinyin_, !config_.pinyin_initials);
    set_check(hInitials_, config_.pinyin_initials);
    const bool dark = config_.theme == kDarkTheme;
    set_check(hLight_, !dark);
    set_check(hDark_, dark);
    apply_ui_theme(dark);  // also after a change made elsewhere
    const int font = config_.font_size;
    set_check(hFontSmall_, font <= kFontSmall);
    set_check(hFontMedium_, font > kFontSmall && font < kFontLarge);
    set_check(hFontLarge_, font >= kFontLarge);
    set_check(hVertical_, config_.layout != "horizontal");
    set_check(hHorizontal_, config_.layout == "horizontal");

    combo_set_index(hPageSize_,
                    std::clamp(config_.page_size, kMinPageSize, kMaxPageSize) - kMinPageSize);

    int language_index = 0;  // follow system
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i].code == config_.ui_language) {
            language_index = static_cast<int>(i) + 1;
        }
    }
    combo_set_index(hLanguage_, language_index);
    set_check(hEnglishCorrection_, config_.english.correction);
    set_check(hAutostart_, config_.autostart);

    set_switch_key_boxes(config_);

    set_check(hLearning_, config_.candidate_learning);
    set_check(hExperience_, config_.experience_program);
    set_check(hCollectInput_, config_.experience_program && config_.collect_input);
    set_check(hUpdateNotify_, config_.update_notify);
    set_check(hFuzzyEnabled_, config_.fuzzy_pinyin);
    for (int i = 0; i < kFuzzyGroupCount; ++i) {
        set_check(hFuzzyGroups_[i], (config_.fuzzy_groups & (1 << i)) != 0);
    }
    populate_learning();
    update_enabled_controls();
}

bool EditorApp::read_controls(bool report_errors) {
    auto& c = config_;
    c.input_mode = get_check(hWubi_) ? 1 : 0;
    c.pinyin_initials = get_check(hInitials_);
    c.theme = get_check(hDark_) ? kDarkTheme : kLightTheme;
    c.font_size = get_check(hFontSmall_) ? kFontSmall
                  : get_check(hFontLarge_) ? kFontLarge
                                           : kFontMedium;
    c.layout = get_check(hHorizontal_) ? "horizontal" : "vertical";
    c.page_size = std::clamp(combo_index(hPageSize_) + kMinPageSize, kMinPageSize, kMaxPageSize);
    const int language_index = combo_index(hLanguage_);
    c.ui_language = language_index > 0 && language_index <= static_cast<int>(languages_.size())
                        ? languages_[language_index - 1].code
                        : kAutoUiLanguage;
    c.english.correction = get_check(hEnglishCorrection_);
    c.autostart = get_check(hAutostart_);
    apply_switch_key_choice(c, key_capture_get(hSwitchKey_));
    c.candidate_learning = get_check(hLearning_);
    c.experience_program = get_check(hExperience_);
    c.collect_input = c.experience_program && get_check(hCollectInput_);
    c.update_notify = get_check(hUpdateNotify_);
    read_learning(c);
    c.fuzzy_pinyin = get_check(hFuzzyEnabled_);
    c.fuzzy_groups = 0;
    for (int i = 0; i < kFuzzyGroupCount; ++i) {
        if (get_check(hFuzzyGroups_[i])) {
            c.fuzzy_groups |= static_cast<uint8_t>(1 << i);
        }
    }

    // The boxes refuse repeated keys; the checks below guard the saved file.
    Config keys = c;
    keys.english_style_shortcut = combo_of(key_capture_get(hStyleKey_));
    keys.punct_toggle_shortcut = combo_of(key_capture_get(hPunctKey_));
    keys.shape_toggle_shortcut = combo_of(key_capture_get(hShapeKey_));
    if (!switch_keys_valid(keys)) {
        if (report_errors) {
            const char* message = "keys.same";
            for (const KeyboardShortcut& shortcut :
                 {keys.ascii_toggle_shortcut, keys.english_style_shortcut,
                  keys.punct_toggle_shortcut, keys.shape_toggle_shortcut}) {
                if (!shortcut.enabled()) continue;
                if (!is_valid_input_mode_shortcut(shortcut)) message = "keys.invalid";
                else if (shortcut == keys.activate_ime_shortcut) message = "keys.conflict";
            }
            MessageBoxW(hwnd_, tr(message), tr("window.title"), MB_OK | MB_ICONERROR);
        }
        return false;
    }
    c.english_style_shortcut = keys.english_style_shortcut;
    c.punct_toggle_shortcut = keys.punct_toggle_shortcut;
    c.shape_toggle_shortcut = keys.shape_toggle_shortcut;
    return true;
}

void EditorApp::set_switch_key_boxes(const Config& config) {
    key_capture_set(hSwitchKey_, switch_key_choice(config));
    key_capture_set(hStyleKey_, KeyChoice::of(config.english_style_shortcut));
    key_capture_set(hPunctKey_, KeyChoice::of(config.punct_toggle_shortcut));
    key_capture_set(hShapeKey_, KeyChoice::of(config.shape_toggle_shortcut));
    update_switch_key_notes();
}

// Checked on this machine each time (programs come and go): another program's global hotkey
// wins over the IME, a Windows input method hotkey is taken over by it.
void EditorApp::update_switch_key_notes() {
    for (HWND box : {hSwitchKey_, hStyleKey_, hPunctKey_, hShapeKey_}) {
        if (!box) continue;
        const KeyChoice choice = key_capture_get(box);
        if (choice.kind != KeyChoice::Kind::kCombo) {
            key_capture_set_note(box, KeyNote::kNone, {});
            continue;
        }
        if (taken_by_other_program(choice.combo)) {
            key_capture_set_note(box, KeyNote::kWarning, tr("keys.note_taken"));
            continue;
        }
        const char* system_name = nullptr;
        switch (system_ime_hotkey(choice.combo)) {
        case SystemHotkey::kImeToggle: system_name = "keys.sys_ime_toggle"; break;
        case SystemHotkey::kShape: system_name = "keys.sys_shape"; break;
        case SystemHotkey::kSymbol: system_name = "keys.sys_symbol"; break;
        case SystemHotkey::kLayout: system_name = "keys.sys_layout"; break;
        case SystemHotkey::kOtherIme: system_name = "keys.sys_other_ime"; break;
        case SystemHotkey::kNone: break;
        }
        if (system_name) {
            std::wstring note = tr("keys.note_system");
            const size_t at = note.find(L"{0}");
            if (at != std::wstring::npos) note.replace(at, 3, tr(system_name));
            key_capture_set_note(box, KeyNote::kInfo, note);
        } else if (is_common_program_shortcut(choice.combo)) {
            key_capture_set_note(box, KeyNote::kInfo, tr("keys.note_program"));
        } else {
            key_capture_set_note(box, KeyNote::kNone, {});
        }
    }
}

void EditorApp::restore_default_keys() {
    if (MessageBoxW(hwnd_, tr("keys.restore_confirm"), tr("window.title"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    // Shown in the boxes; saved with the other settings.
    Config defaults = config_;
    reset_switch_keys(defaults);
    set_switch_key_boxes(defaults);
}

void EditorApp::update_enabled_controls() {
    const bool pinyin = get_check(hPinyin_);
    EnableWindow(hFullPinyin_, pinyin);
    EnableWindow(hInitials_, pinyin);
    // The pairs keep their check marks but are grayed out while fuzzy pinyin is off.
    const bool fuzzy = get_check(hFuzzyEnabled_);
    for (HWND group : hFuzzyGroups_) {
        EnableWindow(group, fuzzy);
    }
}

bool EditorApp::handle_command(int control_id, int notification) {
    switch (control_id) {
    case kLightId:
    case kDarkId:
        if (notification == BN_CLICKED) {
            apply_ui_theme(get_check(hDark_));  // shown at once; saved with the others
        }
        return true;
    case kPinyinId:
    case kWubiId:
    case kFuzzyEnabledId:
        if (notification == BN_CLICKED) {
            update_enabled_controls();
        }
        return true;
    case kLanguageId:
        if (notification == CBN_SELCHANGE) {
            read_controls(false);  // keep the other unsaved choices
            const std::string language = resolve_ui_language(config_.ui_language);
            if (language != ui_language_) {
                ui_language_ = language;
                load_ui_strings(ui_language_);
                rebuild_ui();
            }
        }
        return true;
    case kExportBackupId:
        export_user_backup();
        return true;
    case kImportBackupId:
        import_user_backup();
        return true;
    case kClearLearningId:
        if (notification == BN_CLICKED) {
            clear_learning_data();
        }
        return true;
    case kRestoreKeysId:
        if (notification == BN_CLICKED) {
            restore_default_keys();
        }
        return true;
    case kExperienceId:
    case kCollectInputId:
        if (notification == BN_CLICKED) {
            on_privacy_check(control_id);
        }
        return true;
    case kOpenLogsId:
        if (notification == BN_CLICKED) {
            open_log_folder();
        }
        return true;
    case kDeleteLogsId:
        if (notification == BN_CLICKED) {
            delete_experience_log();
        }
        return true;
    case kSwitchKeyId:
    case kStyleKeyId:
    case kPunctKeyId:
    case kShapeKeyId:
        if (notification == kKeyCaptureChanged) {
            update_switch_key_notes();
        }
        return true;
    default:
        return handle_learning_command(control_id, notification) ||
               handle_update_command(control_id, notification);
    }
}

void EditorApp::clear_learning_data() {
    if (MessageBoxW(hwnd_, tr("dictionary.clear_confirm"), tr("window.title"),
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    if (!ensure_server_running()) {
        return;
    }
    LexiconControlClient client;
    LexiconControlResult result;
    const bool cleared = client.clear_preferences(UserDictKind::PINYIN, &result) &&
                         client.clear_preferences(UserDictKind::WUBI, &result);
    MessageBoxW(hwnd_, cleared ? tr("dictionary.clear_done") : tr("dictionary.clear_failed"),
                tr("window.title"), MB_OK | (cleared ? MB_ICONINFORMATION : MB_ICONERROR));
}

} // namespace settings
} // namespace cxxime
