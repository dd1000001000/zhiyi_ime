// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Settings pages: General (Chinese input, pinyin style, theme, font size, UI language),
// Keys (Chinese/English switch, style switch shortcut) and Dictionary (self-learning).

#include "editor_app.h"

#include <algorithm>
#include <cwchar>

#include <cxxime/keyboard_shortcut.h>
#include <cxxime/lexicon_control.h>
#include <cxxime/user_dict.h>

#include "editor_app_internal.h"

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
    kSwitchKeyId = 1101,
    kStyleEnabledId,
    kStyleKeyId,
    kLearningId = 1201,
    kClearLearningId,
};

constexpr char kLightTheme[] = "moon_light";
constexpr char kDarkTheme[] = "moon_dark";
constexpr int kFontSmall = 12;
constexpr int kFontMedium = 14;
constexpr int kFontLarge = 17;
constexpr KeyboardShortcut kDefaultStyleShortcut = {kKeyModifierControl, VK_SPACE};

// Chinese/English switch key choices (combo order) -> ascii_composer switch_key actions.
enum SwitchKey { kSwitchShift = 0, kSwitchCtrl = 1, kSwitchNone = 2 };

// Width of the right-aligned label column: the widest label of the page (labels differ in
// length between languages).
int label_width(std::initializer_list<const char*> keys) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old_font = SelectObject(dc, get_font());
    int width = 0;
    for (const char* key : keys) {
        const wchar_t* text = tr(key);
        SIZE size = {};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        width = (std::max)(width, static_cast<int>(size.cx));
    }
    SelectObject(dc, old_font);
    ReleaseDC(nullptr, dc);
    return width + S(8);
}

HWND make_button(int id, const wchar_t* text, int x, int y, int width, HWND parent) {
    HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y,
                                   width, S(28), parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return control;
}

void set_shortcut_control(HWND control, const KeyboardShortcut& shortcut) {
    SendMessageW(control, HKM_SETHOTKEY,
                 MAKEWORD(static_cast<BYTE>(shortcut.virtual_key),
                          static_cast<BYTE>(shortcut.modifiers)),
                 0);
}

KeyboardShortcut shortcut_from_control(HWND control) {
    const WORD value = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    return {HIBYTE(value) & kShortcutModifierMask, LOBYTE(value)};
}

bool switch_action_enabled(const Config& config, const char* key) {
    const auto found = config.ascii_switch_key.find(key);
    return found != config.ascii_switch_key.end() && found->second != "noop";
}

int switch_key_choice(const Config& config) {
    if (switch_action_enabled(config, "Shift_L") || switch_action_enabled(config, "Shift_R")) {
        return kSwitchShift;
    }
    if (switch_action_enabled(config, "Control_L") || switch_action_enabled(config, "Control_R")) {
        return kSwitchCtrl;
    }
    return kSwitchNone;
}

void apply_switch_key_choice(Config& config, int choice) {
    // "code": the keys typed so far are committed as letters, then the mode switches.
    const char* shift = choice == kSwitchShift ? "code" : "noop";
    const char* ctrl = choice == kSwitchCtrl ? "code" : "noop";
    config.ascii_switch_key["Shift_L"] = shift;
    config.ascii_switch_key["Shift_R"] = shift;
    config.ascii_switch_key["Control_L"] = ctrl;
    config.ascii_switch_key["Control_R"] = ctrl;
}

} // namespace

HWND EditorApp::make_hint(const wchar_t* text, int x, int y, int width, HWND parent) {
    RECT panel_rect = {};
    GetClientRect(parent, &panel_rect);
    width = (std::min)(width, static_cast<int>(panel_rect.right) - x - S(8));
    if (!hHintFont_) {
        hHintFont_ = CreateFontW(-S(kFontPt - 2), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    }
    // Two lines: long hints wrap.
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y,
                                   width, S(2 * (kFontPt + 6)), parent, nullptr,
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(hHintFont_), TRUE);
    hints_.push_back(control);
    return control;
}

void EditorApp::create_general_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    const int labels = label_width({"general.chinese_input", "general.pinyin_style",
                                    "general.theme", "general.font_size", "general.language"});
    int y = kPanelPadTop;
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
    make_hint(tr("general.pinyin_style_hint"), x, y - S(6), S(380), panel);
    y += kRowH + S(8);
    radios("general.theme", {{kLightId, "general.light"}, {kDarkId, "general.dark"}},
           {&hLight_, &hDark_});
    radios("general.font_size",
           {{kFontSmallId, "general.small"}, {kFontMediumId, "general.medium"},
            {kFontLargeId, "general.large"}},
           {&hFontSmall_, &hFontMedium_, &hFontLarge_});

    const int combo_x = make_aligned_label(tr("general.language"), x0, labels, y, panel);
    hLanguage_ = make_combo(kLanguageId, combo_x, y, S(220), panel);
    combo_add(hLanguage_, tr("general.follow_system"));
    languages_ = available_ui_languages();
    for (const UiLanguage& language : languages_) {
        combo_add(hLanguage_, language.name.c_str());
    }
}

void EditorApp::create_keys_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = kPanelPadTop;
    const int labels = label_width({"keys.switch", "keys.style"});
    int x = make_aligned_label(tr("keys.switch"), x0, labels, y, panel);
    hSwitchKey_ = make_combo(kSwitchKeyId, x, y, S(220), panel);
    combo_add(hSwitchKey_, L"Shift");
    combo_add(hSwitchKey_, L"Ctrl");
    combo_add(hSwitchKey_, tr("keys.none"));
    y += kRowH;

    x = make_aligned_label(tr("keys.style"), x0, labels, y, panel);
    hStyleEnabled_ = make_check(kStyleEnabledId, tr("keys.enable"), x, y, S(80), panel);
    hStyleKey_ = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, x + S(86), y, S(180), kCtrlH,
                                 panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStyleKeyId)),
                                 GetModuleHandle(nullptr), nullptr);
    SendMessageW(hStyleKey_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    y += kRowH;
    make_hint(tr("keys.style_hint"), x, y - S(6), S(380), panel);
}

void EditorApp::create_dictionary_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = kPanelPadTop;
    hLearning_ = make_check(kLearningId, tr("dictionary.learning"), x0, y, S(400), panel);
    y += kRowH;
    make_hint(tr("dictionary.learning_hint"), x0 + S(20), y - S(6), S(420), panel);
    y += kRowH + S(8);
    make_button(kClearLearningId, tr("dictionary.clear"), x0, y, S(160), panel);
}

void EditorApp::populate_controls() {
    set_check(hPinyin_, config_.input_mode != 1);
    set_check(hWubi_, config_.input_mode == 1);
    set_check(hFullPinyin_, !config_.pinyin_initials);
    set_check(hInitials_, config_.pinyin_initials);
    const bool dark = config_.theme == kDarkTheme;
    set_check(hLight_, !dark);
    set_check(hDark_, dark);
    const int font = config_.font_size;
    set_check(hFontSmall_, font <= kFontSmall);
    set_check(hFontMedium_, font > kFontSmall && font < kFontLarge);
    set_check(hFontLarge_, font >= kFontLarge);

    int language_index = 0;  // follow system
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i].code == config_.ui_language) {
            language_index = static_cast<int>(i) + 1;
        }
    }
    combo_set_index(hLanguage_, language_index);

    combo_set_index(hSwitchKey_, switch_key_choice(config_));
    set_check(hStyleEnabled_, config_.english_style_shortcut.enabled());
    set_shortcut_control(hStyleKey_, config_.english_style_shortcut.enabled()
                                         ? config_.english_style_shortcut
                                         : kDefaultStyleShortcut);

    set_check(hLearning_, config_.candidate_learning);
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
    const int language_index = combo_index(hLanguage_);
    c.ui_language = language_index > 0 && language_index <= static_cast<int>(languages_.size())
                        ? languages_[language_index - 1].code
                        : kAutoUiLanguage;
    apply_switch_key_choice(c, (std::max)(0, combo_index(hSwitchKey_)));
    c.candidate_learning = get_check(hLearning_);

    KeyboardShortcut style_shortcut;
    if (get_check(hStyleEnabled_)) {
        style_shortcut = shortcut_from_control(hStyleKey_);
        if (!style_shortcut.enabled() || !is_valid_input_mode_shortcut(style_shortcut)) {
            if (report_errors) {
                MessageBoxW(hwnd_, tr("keys.invalid"), tr("window.title"), MB_OK | MB_ICONERROR);
            }
            return false;
        }
        if (style_shortcut == c.activate_ime_shortcut) {
            if (report_errors) {
                MessageBoxW(hwnd_, tr("keys.conflict"), tr("window.title"), MB_OK | MB_ICONERROR);
            }
            return false;
        }
    }
    c.english_style_shortcut = style_shortcut;
    return true;
}

void EditorApp::update_enabled_controls() {
    const bool pinyin = get_check(hPinyin_);
    EnableWindow(hFullPinyin_, pinyin);
    EnableWindow(hInitials_, pinyin);
    EnableWindow(hStyleKey_, get_check(hStyleEnabled_));
}

bool EditorApp::handle_command(int control_id, int notification) {
    switch (control_id) {
    case kPinyinId:
    case kWubiId:
    case kStyleEnabledId:
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
    case kClearLearningId:
        if (notification == BN_CLICKED) {
            clear_learning_data();
        }
        return true;
    default:
        return false;
    }
}

void EditorApp::clear_learning_data() {
    if (MessageBoxW(hwnd_, tr("dictionary.clear_confirm"), tr("window.title"),
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
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
