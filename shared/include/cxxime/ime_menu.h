// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Modified by Zhiyi IME Contributors: the right-click menu of the taskbar 中/英 indicator
// switches the input states (Chinese/English, input method, English style, punctuation,
// full/half width) and can exit the background service; the floating status window was
// removed.

#ifndef CXXIME_IME_MENU_H_
#define CXXIME_IME_MENU_H_

#include <cstdint>
#include <string>

#include <windows.h>

#include <cxxime/ipc_protocol.h>

namespace cxxime {

enum class ImeMenuCommand : uint32_t {
    kPinyin = 1,  // full pinyin
    kWubi = 2,
    kMixed = 3,   // not in the menu
    kDictionary = 4,
    // 5 was the floating status window switch.
    kSettings = 6,
    kAbout = 7,
    kChinese = 8,
    kEnglish = 9,
    kPinyinInitials = 10,
    kEnglishWords = 11,  // English mode: word completion (else letter by letter)
    kChinesePunct = 12,
    kFullShape = 13,
    kExit = 14,  // closes zhiyi-server; switching to the input method again starts it
};

struct ImeMenuItem {
    ImeMenuCommand command;
    const wchar_t* label;     // Chinese
    const wchar_t* label_en;  // English
    bool starts_group;
};

inline constexpr ImeMenuItem kImeMenuItems[] = {
    {ImeMenuCommand::kChinese, L"中文", L"Chinese", false},
    {ImeMenuCommand::kEnglish, L"英文", L"English", false},
    {ImeMenuCommand::kPinyin, L"全拼", L"Full pinyin", true},
    {ImeMenuCommand::kPinyinInitials, L"首字母", L"Pinyin initials", false},
    {ImeMenuCommand::kWubi, L"五笔", L"Wubi", false},
    {ImeMenuCommand::kEnglishWords, L"英文单词联想", L"English word completion", true},
    {ImeMenuCommand::kChinesePunct, L"中文标点", L"Chinese punctuation", false},
    {ImeMenuCommand::kFullShape, L"全角", L"Full width", false},
    {ImeMenuCommand::kSettings, L"设置…", L"Settings…", true},
    {ImeMenuCommand::kAbout, L"关于", L"About", false},
    {ImeMenuCommand::kExit, L"退出", L"Exit", true},
};

// The IME interface language (Config::ui_language: zh-CN, en-US, or auto = the Windows
// display language): Chinese, or English for any other language.
inline bool ime_menu_uses_chinese(const std::string& ui_language) {
    if (ui_language == "zh-CN") return true;
    if (ui_language == "en-US") return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

inline const wchar_t* ime_menu_item_label(const ImeMenuItem& item, bool chinese) {
    return chinese ? item.label : item.label_en;
}

inline bool ime_menu_command_checked(ImeMenuCommand command, const ImeStatus& status) {
    const bool pinyin = status.input_mode == InputMode::PINYIN;
    switch (command) {
    case ImeMenuCommand::kChinese: return status.chinese_mode();
    case ImeMenuCommand::kEnglish: return !status.chinese_mode();
    case ImeMenuCommand::kPinyin: return pinyin && !status.pinyin_initials();
    case ImeMenuCommand::kPinyinInitials: return pinyin && status.pinyin_initials();
    case ImeMenuCommand::kWubi: return status.input_mode == InputMode::WUBI;
    case ImeMenuCommand::kEnglishWords: return status.english_words();
    case ImeMenuCommand::kChinesePunct: return status.chinese_punct();
    case ImeMenuCommand::kFullShape: return status.full_shape();
    default: return false;
    }
}

// The tooltip of the indicator: every state at a glance, e.g. "中文 · 全拼 · 半角 · 中文标点".
inline std::wstring ime_status_summary(const ImeStatus& status, bool chinese) {
    std::wstring text;
    if (status.caps_lock()) {
        text = chinese ? L"大写锁定" : L"Caps Lock";
    } else if (status.chinese_mode()) {
        text = chinese ? L"中文" : L"Chinese";
    } else {
        text = status.english_words() ? (chinese ? L"英文（单词联想）" : L"English (words)")
                                      : (chinese ? L"英文（逐字母）" : L"English (letters)");
    }
    text += L" · ";
    if (status.input_mode == InputMode::WUBI) {
        text += chinese ? L"五笔" : L"Wubi";
    } else {
        text += status.pinyin_initials() ? (chinese ? L"首字母" : L"Initials")
                                         : (chinese ? L"全拼" : L"Full pinyin");
    }
    text += L" · ";
    text += status.full_shape() ? (chinese ? L"全角" : L"Full width")
                                : (chinese ? L"半角" : L"Half width");
    text += L" · ";
    text += status.chinese_punct() ? (chinese ? L"中文标点" : L"Chinese punctuation")
                                   : (chinese ? L"英文标点" : L"English punctuation");
    return text;
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
