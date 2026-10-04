// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <string>

#include "settings_launcher_util.h"
#include "support/testutil.h"

TEST(SettingsLauncher, accepts_absolute_install_location) {
    const wchar_t value[] = L"C:\\Program Files\\ZhiyiIME\\0.5.0\\";
    std::wstring path;
    ASSERT_TRUE(cxxime::build_registered_settings_path(value, sizeof(value), &path));
    ASSERT_EQ(path, std::wstring(L"C:\\Program Files\\ZhiyiIME\\0.5.0\\zhiyi-settings.exe"));
}

TEST(SettingsLauncher, accepts_drive_root) {
    const wchar_t value[] = L"D:\\";
    std::wstring path;
    ASSERT_TRUE(cxxime::build_registered_settings_path(value, sizeof(value), &path));
    ASSERT_EQ(path, std::wstring(L"D:\\zhiyi-settings.exe"));
}

TEST(SettingsLauncher, rejects_embedded_or_extra_nulls) {
    const wchar_t embedded[] = {L'C', L':', L'\\', L'a', L'p', L'p', L'\0', L'x', L'\0'};
    const wchar_t extra[] = {L'C', L':', L'\\', L'a', L'p', L'p', L'\0', L'\0'};
    std::wstring path = L"unchanged";
    ASSERT_TRUE(!cxxime::build_registered_settings_path(embedded, sizeof(embedded), &path));
    ASSERT_TRUE(path.empty());
    ASSERT_TRUE(!cxxime::build_registered_settings_path(extra, sizeof(extra), &path));
}

TEST(SettingsLauncher, rejects_invalid_registry_strings) {
    const wchar_t relative[] = L"ZhiyiIME\\0.5.0";
    const wchar_t drive_relative[] = L"C:ZhiyiIME";
    const wchar_t unc[] = L"\\\\server\\ZhiyiIME";
    const wchar_t separators[] = L"\\\\";
    const wchar_t unterminated[] = {L'C', L':', L'\\'};
    std::wstring path;
    ASSERT_TRUE(!cxxime::build_registered_settings_path(relative, sizeof(relative), &path));
    ASSERT_TRUE(
        !cxxime::build_registered_settings_path(drive_relative, sizeof(drive_relative), &path));
    ASSERT_TRUE(!cxxime::build_registered_settings_path(unc, sizeof(unc), &path));
    ASSERT_TRUE(!cxxime::build_registered_settings_path(separators, sizeof(separators), &path));
    ASSERT_TRUE(!cxxime::build_registered_settings_path(unterminated, sizeof(unterminated), &path));
    ASSERT_TRUE(!cxxime::build_registered_settings_path(relative, sizeof(relative) - 1, &path));
}

TEST(SettingsLauncher, compares_normalized_paths_case_insensitively) {
    ASSERT_TRUE(cxxime::settings_paths_equal(L"C:\\Program Files\\ZhiyiIME\\0.5.0\\x.exe",
                                             L"c:\\program files\\zhiyiime\\0.5.0\\x.exe"));
    ASSERT_TRUE(cxxime::settings_paths_equal(L"C:\\Program Files\\ZhiyiIME\\.\\x.exe",
                                             L"C:\\Program Files\\ZhiyiIME\\x.exe"));
    ASSERT_TRUE(!cxxime::settings_paths_equal(L"C:\\ZhiyiIME\\stable\\x.exe",
                                              L"C:\\ZhiyiIME\\preview\\x.exe"));
}

RUN_ALL_TESTS()
