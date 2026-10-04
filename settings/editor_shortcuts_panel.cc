// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <string>

#include <cxxime/keyboard_shortcut.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kHotkeyProbeId = 0x3e51;
static_assert(kKeyModifierShift == HOTKEYF_SHIFT,
              "Shortcut modifier values must match hotkey control");
static_assert(kKeyModifierControl == HOTKEYF_CONTROL,
              "Shortcut modifier values must match hotkey control");
static_assert(kKeyModifierAlt == HOTKEYF_ALT,
              "Shortcut modifier values must match hotkey control");
constexpr KeyboardShortcut kSuggestedInputModeShortcut = {0, VK_F4};
constexpr KeyboardShortcut kSuggestedEnglishStyleShortcut = {kKeyModifierControl, VK_SPACE};
constexpr KeyboardShortcut kSuggestedActivateImeShortcut = {
    kKeyModifierControl | kKeyModifierAlt,
    'M',
};

struct ShortcutOption {
    const wchar_t* label;
    const char* value;
};

const ShortcutOption kModifierShortcutOptions[] = {
    {L"临时英文 (inline_ascii)", "inline_ascii"},
    {L"提交编码并切换 (code)", "code"},
    {L"提交首选并切换 (candidate)", "candidate"},
    {L"清空编码并切换 (clear)", "clear"},
    {L"切换到英文 (set_ascii_mode)", "set_ascii_mode"},
    {L"切换到中文 (unset_ascii_mode)", "unset_ascii_mode"},
    {L"不处理 (noop)", "noop"},
};

const ShortcutOption kCapsLockShortcutOptions[] = {
    {L"提交编码 (code)", "code"},   {L"提交首选 (candidate)", "candidate"},
    {L"清空编码 (clear)", "clear"}, {L"保留大小写输入 (append)", "append"},
    {L"不处理 (noop)", "noop"},
};

void add_shortcut_options(HWND combo, const ShortcutOption* options, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        LRESULT index =
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(options[i].label));
        if (index >= 0) {
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(options[i].value));
        }
    }
}

void select_shortcut_option(HWND combo, const std::string& value) {
    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        auto* item_value = reinterpret_cast<const char*>(
            SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if (item_value && value == item_value) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(count - 1), 0);
}

const char* selected_shortcut_option(HWND combo) {
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0) {
        return nullptr;
    }
    LRESULT value = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    return value == CB_ERR ? nullptr : reinterpret_cast<const char*>(value);
}

void set_shortcut_control(HWND control, const KeyboardShortcut& shortcut) {
    const WORD control_value =
        MAKEWORD(static_cast<BYTE>(shortcut.virtual_key), static_cast<BYTE>(shortcut.modifiers));
    SendMessageW(control, HKM_SETHOTKEY, control_value, 0);
}

KeyboardShortcut shortcut_from_control(HWND control) {
    const WORD control_value = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    return {HIBYTE(control_value) & kShortcutModifierMask, LOBYTE(control_value)};
}

bool activate_ime_shortcut_is_available(HWND window, const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) {
        return true;
    }
    if (!is_valid_activate_ime_shortcut(shortcut) ||
        !RegisterHotKey(window, kHotkeyProbeId, keyboard_shortcut_win32_modifiers(shortcut),
                        shortcut.virtual_key)) {
        return false;
    }
    UnregisterHotKey(window, kHotkeyProbeId);
    return true;
}

} // namespace

void EditorApp::create_shortcuts_panel(HWND panel) {
    const int top = kPanelPadTop;
    const int label_width = S(130);
    const wchar_t* names[] = {
        L"左 Shift:", L"右 Shift:", L"左 Ctrl:", L"右 Ctrl:", L"Caps Lock 行为:",
    };
    SetWindowSubclass(panel, PanelForwardProc, 3000, reinterpret_cast<DWORD_PTR>(hwnd_));

    for (int i = 0; i < 4; ++i) {
        int control_x =
            make_aligned_label(names[i], kPanelPadLeft, label_width, top + i * kRowH, panel);
        hKeyCombos_[i] = make_combo(1300 + i, control_x, top + i * kRowH, S(300), panel);
        add_shortcut_options(hKeyCombos_[i], kModifierShortcutOptions,
                             _countof(kModifierShortcutOptions));
    }

    const int caps_lock_index = 4;
    int control_x = make_aligned_label(names[caps_lock_index], kPanelPadLeft, label_width,
                                       top + caps_lock_index * kRowH, panel);
    hKeyCombos_[caps_lock_index] =
        make_combo(1300 + caps_lock_index, control_x, top + caps_lock_index * kRowH, S(300), panel);
    add_shortcut_options(hKeyCombos_[caps_lock_index], kCapsLockShortcutOptions,
                         _countof(kCapsLockShortcutOptions));

    control_x =
        make_aligned_label(L"切换输入模式:", kPanelPadLeft, label_width, top + 5 * kRowH, panel);
    hInputModeSwitchEnabled_ = make_check(1305, L"启用", control_x, top + 5 * kRowH, S(62), panel);
    hInputModeSwitchKey_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, control_x + S(68),
        top + 5 * kRowH, S(232), kCtrlH, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1306)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(hInputModeSwitchKey_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    control_x =
        make_aligned_label(L"切换到知意输入法:", kPanelPadLeft, label_width, top + 6 * kRowH, panel);
    hActivateImeHotkeyEnabled_ =
        make_check(1307, L"启用", control_x, top + 6 * kRowH, S(62), panel);
    hActivateImeHotkey_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, control_x + S(68),
        top + 6 * kRowH, S(232), kCtrlH, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1308)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(hActivateImeHotkey_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    control_x =
        make_aligned_label(L"英文单词/字母:", kPanelPadLeft, label_width, top + 7 * kRowH, panel);
    hEnglishStyleEnabled_ = make_check(1309, L"启用", control_x, top + 7 * kRowH, S(62), panel);
    hEnglishStyleKey_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, control_x + S(68),
        top + 7 * kRowH, S(232), kCtrlH, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1310)),
        GetModuleHandle(nullptr), nullptr);
    SendMessageW(hEnglishStyleKey_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
}

bool EditorApp::handle_shortcuts_command(int control_id, int notification) {
    if (control_id != 1305 && control_id != 1307 && control_id != 1309) {
        return false;
    }
    if (notification == BN_CLICKED) {
        update_shortcut_controls_enabled();
    }
    return true;
}

void EditorApp::load_shortcut_controls() {
    const char* keys[] = {"Shift_L", "Shift_R", "Control_L", "Control_R", "Caps_Lock"};
    for (int i = 0; i < 5; ++i) {
        auto entry = config_.ascii_switch_key.find(keys[i]);
        std::string value = entry != config_.ascii_switch_key.end() ? entry->second : "noop";
        select_shortcut_option(hKeyCombos_[i], value);
    }
    set_check(hInputModeSwitchEnabled_, config_.input_mode_switch_shortcut.enabled());
    set_shortcut_control(hInputModeSwitchKey_, config_.input_mode_switch_shortcut.enabled()
                             ? config_.input_mode_switch_shortcut
                             : kSuggestedInputModeShortcut);
    set_check(hActivateImeHotkeyEnabled_, config_.activate_ime_shortcut.enabled());
    set_shortcut_control(hActivateImeHotkey_, config_.activate_ime_shortcut.enabled()
                             ? config_.activate_ime_shortcut
                             : kSuggestedActivateImeShortcut);
    set_check(hEnglishStyleEnabled_, config_.english_style_shortcut.enabled());
    set_shortcut_control(hEnglishStyleKey_, config_.english_style_shortcut.enabled()
                             ? config_.english_style_shortcut
                             : kSuggestedEnglishStyleShortcut);
    update_shortcut_controls_enabled();
}

bool EditorApp::read_shortcut_controls() {
    const char* keys[] = {"Shift_L", "Shift_R", "Control_L", "Control_R", "Caps_Lock"};
    for (int i = 0; i < 5; ++i) {
        const char* value = selected_shortcut_option(hKeyCombos_[i]);
        if (value) {
            config_.ascii_switch_key[keys[i]] = value;
        }
    }

    KeyboardShortcut input_mode_shortcut;
    if (get_check(hInputModeSwitchEnabled_)) {
        input_mode_shortcut = shortcut_from_control(hInputModeSwitchKey_);
        if (!input_mode_shortcut.enabled() || !is_valid_input_mode_shortcut(input_mode_shortcut)) {
            MessageBoxW(hwnd_,
                        L"输入模式快捷键可使用 F1-F11，或使用 Ctrl/Alt（可同时搭配 "
                        L"Shift）与字母、数字、常用标点或 Space 组合。",
                        L"知意输入法设置", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    KeyboardShortcut activate_ime_shortcut;
    if (get_check(hActivateImeHotkeyEnabled_)) {
        activate_ime_shortcut = shortcut_from_control(hActivateImeHotkey_);
        if (!activate_ime_shortcut.enabled() ||
            !is_valid_activate_ime_shortcut(activate_ime_shortcut)) {
            MessageBoxW(hwnd_,
                        L"全局快捷键可使用 F1-F11，或使用 Ctrl/Alt（可同时搭配 "
                        L"Shift）与字母、数字、常用标点或 Space 组合。",
                        L"知意输入法设置", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    KeyboardShortcut english_style_shortcut;
    if (get_check(hEnglishStyleEnabled_)) {
        english_style_shortcut = shortcut_from_control(hEnglishStyleKey_);
        if (!english_style_shortcut.enabled() ||
            !is_valid_input_mode_shortcut(english_style_shortcut)) {
            MessageBoxW(hwnd_,
                        L"英文单词/字母切换键可使用 F1-F11，或使用 Ctrl/Alt（可同时搭配 "
                        L"Shift）与字母、数字、常用标点或 Space 组合。",
                        L"知意输入法设置", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    if ((input_mode_shortcut.enabled() && input_mode_shortcut == activate_ime_shortcut) ||
        (english_style_shortcut.enabled() && (english_style_shortcut == input_mode_shortcut ||
                                              english_style_shortcut == activate_ime_shortcut))) {
        MessageBoxW(hwnd_, L"快捷键之间不能使用相同的按键组合。", L"知意输入法设置",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (activate_ime_shortcut != config_.activate_ime_shortcut &&
        !activate_ime_shortcut_is_available(hwnd_, activate_ime_shortcut)) {
        MessageBoxW(hwnd_, L"该全局快捷键已被其他程序占用。", L"知意输入法设置",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    config_.input_mode_switch_shortcut = input_mode_shortcut;
    config_.activate_ime_shortcut = activate_ime_shortcut;
    config_.english_style_shortcut = english_style_shortcut;
    return true;
}

void EditorApp::update_shortcut_controls_enabled() {
    EnableWindow(hInputModeSwitchKey_, get_check(hInputModeSwitchEnabled_));
    EnableWindow(hActivateImeHotkey_, get_check(hActivateImeHotkeyEnabled_));
    EnableWindow(hEnglishStyleKey_, get_check(hEnglishStyleEnabled_));
}

} // namespace settings
} // namespace cxxime
