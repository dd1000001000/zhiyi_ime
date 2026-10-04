// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_IME_MENU_H_
#define CXXIME_IME_MENU_H_

#include <cstdint>

#include <windows.h>

#include <cxxime/ipc_protocol.h>

namespace cxxime {

enum class ImeMenuCommand : uint32_t {
    kPinyin = 1,
    kWubi = 2,
    kMixed = 3,
    kDictionary = 4,
    kToggleStatusWindow = 5,
    kSettings = 6,
    kAbout = 7,
};

struct ImeMenuItem {
    ImeMenuCommand command;
    const wchar_t* label;     // Chinese
    const wchar_t* label_en;  // English
    bool starts_group;
};

inline constexpr ImeMenuItem kImeMenuItems[] = {
    {ImeMenuCommand::kPinyin, L"拼音", L"Pinyin", false},
    {ImeMenuCommand::kWubi, L"五笔", L"Wubi", false},
    {ImeMenuCommand::kToggleStatusWindow, nullptr, nullptr, true},
    {ImeMenuCommand::kSettings, L"设置", L"Settings", false},
    {ImeMenuCommand::kAbout, L"关于", L"About", true},
};

// Menu labels follow the Windows display language: Chinese, or English for any other language.
inline bool ime_menu_uses_chinese() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

inline const wchar_t* ime_menu_item_label(const ImeMenuItem& item,
                                           bool status_window_visible) {
    const bool chinese = ime_menu_uses_chinese();
    if (item.command == ImeMenuCommand::kToggleStatusWindow) {
        if (chinese) {
            return status_window_visible ? L"隐藏状态窗口" : L"显示状态窗口";
        }
        return status_window_visible ? L"Hide status window" : L"Show status window";
    }
    return chinese ? item.label : item.label_en;
}

inline bool ime_menu_command_checked(ImeMenuCommand command, InputMode input_mode) {
    return (command == ImeMenuCommand::kPinyin && input_mode == InputMode::PINYIN) ||
           (command == ImeMenuCommand::kWubi && input_mode == InputMode::WUBI);
}

inline const ImeMenuItem* find_ime_menu_item(uint32_t command_id) {
    for (const ImeMenuItem& item : kImeMenuItems) {
        if (static_cast<uint32_t>(item.command) == command_id) {
            return &item;
        }
    }
    return nullptr;
}

} // namespace cxxime

#endif // CXXIME_IME_MENU_H_
