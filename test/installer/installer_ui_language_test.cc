// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

// The installer language becomes the settings language (user config ui.language).

#include <fstream>
#include <iterator>
#include <string>

#include <windows.h>

#include <cxxime/installer_ui_language.h>

#include "support/testutil.h"

namespace {

std::wstring temp_dir(const wchar_t* name) {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + name;
}

void write_text(const std::wstring& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string read_text(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

TEST(InstallerUiLanguage, langid_maps_chinese_and_everything_else) {
    using cxxime::installer::ui_language_for_langid;
    ASSERT_EQ(ui_language_for_langid(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)), "zh-CN");
    ASSERT_EQ(ui_language_for_langid(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)), "zh-CN");
    ASSERT_EQ(ui_language_for_langid(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)), "en-US");
    ASSERT_EQ(ui_language_for_langid(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)), "en-US");
}

TEST(InstallerUiLanguage, first_install_creates_the_user_config) {
    const std::wstring dir = temp_dir(L"zhiyi_ui_language_first");
    const std::wstring path = dir + L"\\default.json";
    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());

    ASSERT_EQ(cxxime::installer::read_ui_language(path), "auto");
    // Same as the system language: nothing to write, the settings keep following the system.
    ASSERT_TRUE(cxxime::installer::write_ui_language(path, "zh-CN", "zh-CN"));
    ASSERT_TRUE(!exists(path));
    // Another language: written, the directory is created.
    ASSERT_TRUE(cxxime::installer::write_ui_language(path, "en-US", "zh-CN"));
    ASSERT_EQ(cxxime::installer::read_ui_language(path), "en-US");

    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST(InstallerUiLanguage, existing_settings_are_kept) {
    const std::wstring dir = temp_dir(L"zhiyi_ui_language_existing");
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\default.json";
    write_text(path, R"({"engine": {"page_size": 9}, "ui": {"language": "auto", "x": 1}})");

    ASSERT_EQ(cxxime::installer::read_ui_language(path), "auto");
    ASSERT_TRUE(cxxime::installer::write_ui_language(path, "zh-CN", "en-US"));
    ASSERT_EQ(cxxime::installer::read_ui_language(path), "zh-CN");
    const std::string text = read_text(path);
    ASSERT_TRUE(text.find("\"page_size\": 9") != std::string::npos);
    ASSERT_TRUE(text.find("\"x\": 1") != std::string::npos);

    // An explicit language is replaced by the newly chosen one, even the system's.
    ASSERT_TRUE(cxxime::installer::write_ui_language(path, "en-US", "en-US"));
    ASSERT_EQ(cxxime::installer::read_ui_language(path), "en-US");

    // A damaged config is not touched.
    write_text(path, "{not json");
    ASSERT_EQ(cxxime::installer::read_ui_language(path), "");
    ASSERT_TRUE(!cxxime::installer::write_ui_language(path, "zh-CN", "en-US"));
    ASSERT_EQ(read_text(path), "{not json");

    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST(InstallerExperienceProgram, answers_are_saved_and_other_settings_kept) {
    const std::wstring dir = temp_dir(L"zhiyi_experience_program");
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\default.json";
    DeleteFileW(path.c_str());

    // No config yet: unset; "off" (the default) leaves no file behind.
    ASSERT_EQ(cxxime::installer::read_privacy(path).experience_program, "unset");
    ASSERT_TRUE(cxxime::installer::write_privacy(path, false, false));
    ASSERT_TRUE(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    write_text(path, R"({"engine": {"page_size": 9}})");
    ASSERT_TRUE(cxxime::installer::write_privacy(path, true, true));
    auto answers = cxxime::installer::read_privacy(path);
    ASSERT_EQ(answers.experience_program, "on");
    ASSERT_EQ(answers.collect_input, "on");
    ASSERT_TRUE(read_text(path).find("\"page_size\": 9") != std::string::npos);

    // Input collection needs the program.
    ASSERT_TRUE(cxxime::installer::write_privacy(path, false, true));
    answers = cxxime::installer::read_privacy(path);
    ASSERT_EQ(answers.experience_program, "off");
    ASSERT_EQ(answers.collect_input, "off");

    write_text(path, "{not json");
    ASSERT_EQ(cxxime::installer::read_privacy(path).experience_program, "");
    ASSERT_TRUE(!cxxime::installer::write_privacy(path, true, false));

    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

RUN_ALL_TESTS()
