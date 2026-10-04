// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

// English word mode: spelling corrections, and learning corrections / words kept as typed.

#include <cmath>
#include <fstream>
#include <string>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/english_candidates.h>
#include <cxxime/english_learning.h>
#include <cxxime/english_lexicon.h>

#include "support/testutil.h"

namespace {

std::string temp_file(const char* name) {
    char directory[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, directory);
    return std::string(directory) + name;
}

bool close_to(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A small word list in the english.words.tsv format (code, text, 100 * Zipf).
cxxime::EnglishLexicon test_lexicon() {
    const std::string path = temp_file("zhiyi_english_correction.tsv");
    {
        std::ofstream f(path, std::ios::binary);
        f << "the\tthe\t773\n"
             "ten\tten\t505\n"
             "tea\ttea\t473\n"
             "tech\ttech\t469\n"
             "receive\treceive\t485\n"
             "address\taddress\t491\n"
             "beautiful\tbeautiful\t522\n"
             "beautify\tbeautify\t380\n"
             "weird\tweird\t481\n"
             "definitely\tdefinitely\t496\n"
             "form\tform\t531\n"
             "from\tfrom\t663\n"
             "kubectl\tkubectl\t50\n"
             "access\taccess\t520\n"
             "access\tMicrosoft Access\t378\n"
             "theory\ttheory\t470\n"
             "definately\tdefinately\t287\n";
    }
    cxxime::EnglishLexicon lexicon;
    lexicon.load(path);
    DeleteFileA(path.c_str());
    return lexicon;
}

bool has_word(const std::vector<cxxime::EnglishWord>& words, const std::string& text,
              bool corrected) {
    for (const auto& w : words) {
        if (w.text == text && w.corrected == corrected) return true;
    }
    return false;
}

bool any_corrected(const std::vector<cxxime::EnglishWord>& words) {
    for (const auto& w : words) {
        if (w.corrected) return true;
    }
    return false;
}

} // namespace

TEST(EnglishCorrection, typing_cost_weights_common_mistakes) {
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("the", "the"), 0.0f));
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("teh", "the"), 0.75f));         // swapped
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("recieve", "receive"), 0.75f));
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("adress", "address"), 0.5f));   // double letter
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("untill", "until"), 0.5f));
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("teh", "ten"), 0.5f));          // h, n adjacent
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("definately", "definitely"), 1.0f));
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("bhe", "the"), 1.5f));          // first letter
    // A word still being typed: its cheapest beginning.
    ASSERT_TRUE(close_to(cxxime::english_typing_cost("beautf", "beautiful", true), 1.0f));
    ASSERT_TRUE(cxxime::english_typing_cost("beautf", "beautiful") > 2.0f);
}

TEST(EnglishCorrection, correction_limit_grows_with_length) {
    ASSERT_TRUE(close_to(cxxime::english_correction_limit(2), 0.0f));
    ASSERT_TRUE(close_to(cxxime::english_correction_limit(3), 1.0f));
    ASSERT_TRUE(close_to(cxxime::english_correction_limit(6), 1.5f));
    ASSERT_TRUE(close_to(cxxime::english_correction_limit(10), 2.0f));
}

TEST(EnglishCorrection, lexicon_finds_ordinary_words_within_the_limit) {
    const cxxime::EnglishLexicon lexicon = test_lexicon();
    auto teh = lexicon.corrections("teh", 3, 300, false);
    ASSERT_TRUE(!teh.empty());
    ASSERT_EQ(teh.front().text, "the");
    ASSERT_TRUE(teh.front().corrected);
    ASSERT_TRUE(has_word(lexicon.corrections("recieve", 2, 300, false), "receive", true));
    ASSERT_TRUE(has_word(lexicon.corrections("adress", 2, 300, false), "address", true));
    ASSERT_TRUE(has_word(lexicon.corrections("wierd", 2, 300, false), "weird", true));
    ASSERT_TRUE(has_word(lexicon.corrections("definately", 2, 300, false), "definitely", true));

    // Beginnings only with `completions`.
    ASSERT_TRUE(lexicon.corrections("beautf", 2, 300, false).empty());
    auto beautf = lexicon.corrections("beautf", 2, 300, true);
    ASSERT_TRUE(!beautf.empty());
    ASSERT_EQ(beautf.front().text, "beautiful");

    // Expanded names are not corrections; rare words and short codes are not offered.
    auto acess = lexicon.corrections("acess", 5, 300, false);
    ASSERT_TRUE(has_word(acess, "access", true));
    ASSERT_TRUE(!has_word(acess, "Microsoft Access", true));
    ASSERT_TRUE(lexicon.corrections("kubectk", 5, 300, false).empty());  // kubectl scores 50
    ASSERT_TRUE(lexicon.corrections("te", 5, 300, false).empty());
    ASSERT_TRUE(lexicon.corrections("t3h", 5, 300, false).empty());
}

TEST(EnglishCorrection, pool_adds_corrections_only_for_misspellings) {
    const cxxime::EnglishLexicon lexicon = test_lexicon();
    cxxime::Config::EnglishConfig config;

    auto teh = cxxime::build_english_pool(lexicon, nullptr, "teh", config);
    ASSERT_TRUE(has_word(teh.words, "the", true));
    ASSERT_TRUE(teh.learned_pick.empty());
    // The typed case carries over.
    ASSERT_TRUE(has_word(cxxime::build_english_pool(lexicon, nullptr, "Teh", config).words, "The",
                         true));
    // A listed misspelling (rare) does not compete with its correction.
    auto definately = cxxime::build_english_pool(lexicon, nullptr, "definately", config);
    ASSERT_TRUE(!definately.words.empty());
    ASSERT_EQ(definately.words.front().text, "definitely");
    ASSERT_TRUE(!has_word(definately.words, "definately", false));
    // A common word as typed is not corrected (form / from).
    ASSERT_TRUE(!any_corrected(cxxime::build_english_pool(lexicon, nullptr, "form", config).words));
    // At most correction_count corrections.
    config.correction_count = 1;
    int corrections = 0;
    for (const auto& w : cxxime::build_english_pool(lexicon, nullptr, "teh", config).words)
        corrections += w.corrected ? 1 : 0;
    ASSERT_EQ(corrections, 1);
    // Switched off.
    config.correction = false;
    ASSERT_TRUE(!any_corrected(cxxime::build_english_pool(lexicon, nullptr, "teh", config).words));
}

TEST(EnglishLearning, picked_corrections_take_the_recommended_slot_after_two_picks) {
    const cxxime::EnglishLexicon lexicon = test_lexicon();
    const cxxime::Config::EnglishConfig config;
    cxxime::EnglishLearning learning;

    learning.record_correction("teh", "ten");
    auto once = cxxime::build_english_pool(lexicon, &learning, "teh", config);
    ASSERT_TRUE(once.learned_pick.empty());  // one pick: a bonus only
    ASSERT_TRUE(has_word(once.words, "ten", true));

    learning.record_correction("teh", "ten");
    ASSERT_EQ(cxxime::build_english_pool(lexicon, &learning, "teh", config).learned_pick, "ten");
    ASSERT_EQ(cxxime::build_english_pool(lexicon, &learning, "Teh", config).learned_pick, "Ten");
}

TEST(EnglishLearning, words_kept_twice_are_not_corrected_and_complete) {
    const cxxime::EnglishLexicon lexicon = test_lexicon();
    const cxxime::Config::EnglishConfig config;
    cxxime::EnglishLearning learning;

    learning.record_kept("thw", false);
    ASSERT_TRUE(!learning.is_user_word("thw"));  // once may be a slip
    ASSERT_TRUE(any_corrected(cxxime::build_english_pool(lexicon, &learning, "thw", config).words));

    learning.record_kept("thw", false);
    ASSERT_TRUE(learning.is_user_word("THW"));
    ASSERT_TRUE(!any_corrected(cxxime::build_english_pool(lexicon, &learning, "thw", config).words));
    ASSERT_TRUE(!any_corrected(cxxime::build_english_pool(lexicon, &learning, "th", config).words));

    learning.record_kept("useState", false);
    learning.record_kept("useState", false);
    ASSERT_TRUE(has_word(cxxime::build_english_pool(lexicon, &learning, "uses", config).words,
                         "useState", false));
    // Common words are never stored as user words.
    learning.record_kept("from", true);
    learning.record_kept("from", true);
    ASSERT_TRUE(!learning.is_user_word("from"));
    // Too short or not letters.
    learning.record_kept("ab", false);
    learning.record_kept("ab", false);
    ASSERT_TRUE(!learning.is_user_word("ab"));
}

TEST(EnglishLearning, the_latest_habit_wins) {
    cxxime::EnglishLearning learning;
    learning.record_correction("teh", "the");
    learning.record_correction("teh", "the");
    ASSERT_EQ(learning.corrections_for("teh").size(), 1u);
    // Keeping the typed text takes from its corrections and builds a user word.
    learning.record_kept("teh", false);
    ASSERT_EQ(learning.corrections_for("teh").front().count, 1);
    learning.record_kept("teh", false);
    ASSERT_TRUE(learning.corrections_for("teh").empty());
    ASSERT_TRUE(learning.is_user_word("teh"));
    // Picking a correction again takes from the user word.
    learning.record_correction("teh", "the");
    ASSERT_TRUE(!learning.is_user_word("teh"));
}

TEST(EnglishLearning, saves_and_loads) {
    const std::string path = temp_file("zhiyi_learning_english_test.json");
    DeleteFileA(path.c_str());
    {
        cxxime::EnglishLearning learning;
        ASSERT_TRUE(learning.load(path));  // missing file: empty
        learning.record_correction("teh", "the");
        learning.record_correction("teh", "the");
        learning.record_kept("kubectl", false);
        learning.record_kept("kubectl", false);
        ASSERT_TRUE(learning.flush());
    }
    {
        cxxime::EnglishLearning learning;
        ASSERT_TRUE(learning.load(path));
        ASSERT_EQ(learning.corrections_for("teh").front().word, "the");
        ASSERT_EQ(learning.corrections_for("teh").front().count, 2);
        ASSERT_TRUE(learning.is_user_word("kubectl"));
        ASSERT_TRUE(learning.clear_and_save());
        ASSERT_TRUE(learning.corrections_for("teh").empty());
    }
    {
        cxxime::EnglishLearning learning;
        ASSERT_TRUE(learning.load(path));
        ASSERT_TRUE(!learning.is_user_word("kubectl"));
    }
    // A damaged file starts empty.
    {
        std::ofstream f(path, std::ios::binary);
        f << "{not json";
    }
    {
        cxxime::EnglishLearning learning;
        ASSERT_TRUE(!learning.load(path));
        ASSERT_TRUE(learning.corrections_for("teh").empty());
    }
    DeleteFileA(path.c_str());
}

RUN_ALL_TESTS()
