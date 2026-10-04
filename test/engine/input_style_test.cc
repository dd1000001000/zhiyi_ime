// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

// Input styles: pinyin initials matching and the style shortcut (Ctrl+Space by default), which
// switches full pinyin / initials in Chinese pinyin mode and words / letters in English mode.

#include <string>

#include <windows.h>

#include <cxxime/dict.h>
#include <cxxime/engine.h>
#include <cxxime/key_event.h>
#include <cxxime/output_options.h>
#include <cxxime/processor.h>
#include <cxxime/syllabifier.h>

#include "support/testutil.h"

namespace {

std::string temp_file(const char* name) {
    char directory[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, directory);
    return std::string(directory) + name;
}

cxxime::KeyEvent ctrl_space(bool key_up = false) {
    cxxime::KeyEvent event;
    event.keycode = VK_SPACE;
    event.is_key_up = key_up;
    event.set_ctrl();
    return event;
}

} // namespace

TEST(PinyinInitials, each_syllable_matches_one_typed_initial) {
    ASSERT_TRUE(cxxime::pinyin_matches_initials("zgr", "zhong:guo:ren"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("zhgr", "zhong:guo:ren"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("jt", "jin:tian"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("xa", "xi:an"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("a", "a"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("chsh", "chang:shi"));
}

TEST(PinyinInitials, full_syllables_and_other_lengths_do_not_match) {
    // A full syllable is not an initial ("xian" is four initials).
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("xian", "xian"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("xian", "xi:an"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("zg", "zhong:guo:ren"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("zgrm", "zhong:guo:ren"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("hgr", "zhong:guo:ren"));
    // A typed zh is either the initial zh or the initials z and h.
    ASSERT_TRUE(cxxime::pinyin_matches_initials("zhg", "zhong:guo"));
    ASSERT_TRUE(cxxime::pinyin_matches_initials("zhg", "zi:hai:guo"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("chg", "zhong:guo"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("", "zhong"));
    ASSERT_TRUE(!cxxime::pinyin_matches_initials("z", ""));
}

TEST(InputStyle, shortcut_toggles_pinyin_style_in_chinese_and_english_style_in_english) {
    const std::string dict_path = temp_file("zhiyi_input_style.bin");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, {{"ni", "你", 100}}));
    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(dict_path));
    engine.set_trace_enabled(false);

    cxxime::OutputOptions chinese;
    chinese.chinese_mode = true;
    ASSERT_EQ(engine.process_key(ctrl_space(), chinese), cxxime::ProcessResult::TOGGLE_PINYIN_STYLE);
    // Auto-repeat and the matching key-up are consumed without toggling again.
    ASSERT_EQ(engine.process_key(ctrl_space(), chinese),
              cxxime::ProcessResult::INPUT_MODE_SHORTCUT_HANDLED);
    ASSERT_EQ(engine.process_key(ctrl_space(true), chinese),
              cxxime::ProcessResult::INPUT_MODE_SHORTCUT_HANDLED);

    cxxime::OutputOptions english;
    english.chinese_mode = false;
    ASSERT_EQ(engine.process_key(ctrl_space(), english),
              cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE);
    engine.process_key(ctrl_space(true), english);

    // Wubi has no style: the key is left to the application.
    engine.switch_mode(cxxime::InputMode::WUBI);
    if (engine.mode() == cxxime::InputMode::WUBI) {
        ASSERT_TRUE(engine.process_key(ctrl_space(), chinese) !=
                    cxxime::ProcessResult::TOGGLE_PINYIN_STYLE);
    }

    engine.finalize();
    DeleteFileA(dict_path.c_str());
}

TEST(InputStyle, pinyin_style_follows_the_setter) {
    const std::string dict_path = temp_file("zhiyi_input_style_setter.bin");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, {{"ni", "你", 100}}));
    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(dict_path));
    ASSERT_TRUE(!engine.pinyin_initials());
    engine.set_pinyin_initials(true);
    ASSERT_TRUE(engine.pinyin_initials());
    engine.set_pinyin_initials(false);
    ASSERT_TRUE(!engine.pinyin_initials());
    engine.finalize();
    DeleteFileA(dict_path.c_str());
}

RUN_ALL_TESTS()
