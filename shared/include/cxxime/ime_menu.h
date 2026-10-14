// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Modified by Zhiyi IME Contributors: the right-click menu of the taskbar 中/英 indicator
// switches the input states (Chinese/English, input style: full pinyin / initials / Wubi /
// shuangpin, English style, punctuation, full/half width) and can exit the background service;
// the floating status window was removed. The shuangpin scheme is chosen in settings.

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
    kExit = 14,  // closes zhiyu-server; switching to the input method again starts it
    kShuangpin = 15,  // in the scheme chosen in settings (engine.shuangpin_scheme)
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
    {ImeMenuCommand::kShuangpin, L"双拼", L"Shuangpin", false},
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

// ImeStatus::shuangpin_scheme(): the scheme's name.
inline const wchar_t* shuangpin_scheme_name(uint32_t index, bool chinese) {
    static constexpr const wchar_t* kNames[][2] = {
        {L"微软", L"Microsoft"}, {L"小鹤", L"Xiaohe"}, {L"自然码", L"Ziranma"},
        {L"搜狗", L"Sogou"}, {L"智能ABC", L"Smart ABC"}};
    constexpr uint32_t kCount = sizeof(kNames) / sizeof(kNames[0]);
    return kNames[index < kCount ? index : 1u][chinese ? 0 : 1];  // unknown: the default
}

// "双拼（小鹤）": the shuangpin item names the scheme.
inline std::wstring ime_menu_item_label(const ImeMenuItem& item, bool chinese,
                                        const ImeStatus& status) {
    std::wstring label = chinese ? item.label : item.label_en;
    if (item.command == ImeMenuCommand::kShuangpin) {
        label += chinese ? L"（" : L" (";
        label += shuangpin_scheme_name(status.shuangpin_scheme(), chinese);
        label += chinese ? L"）" : L")";
    }
    return label;
}

inline bool ime_menu_command_checked(ImeMenuCommand command, const ImeStatus& status) {
    const bool pinyin = status.input_mode == InputMode::PINYIN;
    switch (command) {
    case ImeMenuCommand::kChinese: return status.chinese_mode();
    case ImeMenuCommand::kEnglish: return !status.chinese_mode();
    case ImeMenuCommand::kPinyin: return pinyin && status.pinyin_style() == PinyinStyle::kFull;
    case ImeMenuCommand::kPinyinInitials:
        return pinyin && status.pinyin_style() == PinyinStyle::kInitials;
    case ImeMenuCommand::kShuangpin:
        return pinyin && status.pinyin_style() == PinyinStyle::kShuangpin;
    case ImeMenuCommand::kWubi: return status.input_mode == InputMode::WUBI;
    case ImeMenuCommand::kEnglishWords: return status.english_words();
    case ImeMenuCommand::kChinesePunct: return status.chinese_punct();
    case ImeMenuCommand::kFullShape: return status.full_shape();
    default: return false;
    }
}

// The tooltip of the indicator: every state at a glance, e.g. "中文 · 全拼 · 半角 · 中文标点"
// or "中文 · 双拼（小鹤） · ...".
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
    } else if (status.pinyin_style() == PinyinStyle::kShuangpin) {
        text += chinese ? L"双拼（" : L"Shuangpin (";
        text += shuangpin_scheme_name(status.shuangpin_scheme(), chinese);
        text += chinese ? L"）" : L")";
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
