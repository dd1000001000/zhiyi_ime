// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SETTINGS_ROUTE_H_
#define CXXIME_SETTINGS_ROUTE_H_

#include <cstdint>

namespace cxxime {

enum class SettingsPanel : uint32_t {
    kInput = 0,
    kCandidate = 1,
    kLayout = 2,
    kShortcuts = 3,
    kDictionary = 4,
    kDiagnostics = 5,
    kAbout = 6,
    kBackup = 7,
};

inline constexpr wchar_t kSettingsWindowTitle[] = L"知意输入法设置";
inline constexpr wchar_t kSettingsWindowClass[] = L"ZhiyiIMESettingsClass5";
inline constexpr wchar_t kSettingsNavigateMessage[] = L"ZhiyiIME.Settings.Navigate";
inline constexpr wchar_t kSettingsPanelArgument[] = L"--panel";
inline constexpr wchar_t kSettingsDictionaryArgument[] = L"dictionary";

} // namespace cxxime

#endif // CXXIME_SETTINGS_ROUTE_H_
