// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>

#include <json.hpp>

#include <cxxime/candidate_preference.h>
#include <cxxime/composition_learning.h>
#include <cxxime/manual_candidate_order.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>
#include <cxxime/user_lexicon.h>

#include "support/testutil.h"

namespace {

struct TempFile {
    std::string path;

    explicit TempFile(const std::string& contents) {
        char directory[MAX_PATH] = {};
        char name[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, directory);
        GetTempFileNameA(directory, "sym", 0, name);
        path = name;
        std::ofstream output(path, std::ios::binary);
        output << contents;
    }

    ~TempFile() { std::remove(path.c_str()); }
};

std::string from_codepoints(const std::string& sequence) {
    std::istringstream input(sequence);
    unsigned int point = 0;
    std::string text;
    while (input >> std::hex >> point) {
        if (point < 0x80) {
            text.push_back(static_cast<char>(point));
        } else if (point < 0x800) {
            text.push_back(static_cast<char>(0xc0 | (point >> 6)));
            text.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        } else if (point < 0x10000) {
            text.push_back(static_cast<char>(0xe0 | (point >> 12)));
            text.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        } else {
            text.push_back(static_cast<char>(0xf0 | (point >> 18)));
            text.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        }
    }
    return text;
}

} // namespace

TEST(OrdinaryCandidate, semantic_policy_preserves_text_and_wire_validation) {
    for (const auto& text : {u8"空格", u8"U盘", "SDK", "C++", "123", u8"25\u2103",
                             u8"开心\U0001f600", u8"说明\u2139", u8"\u2460", u8"\u3071"}) {
        ASSERT_TRUE(cxxime::is_ordinary_candidate_text(text)) << text;
    }
    for (const auto& text : {u8"\u2103", "+-", u8"\u266a\u266b", u8"\U0001f600", u8"1\ufe0f\u20e3",
                             u8"\U0001f1e8\U0001f1f3", u8"\u2139", u8"\u2139\ufe0f"}) {
        ASSERT_TRUE(cxxime::is_valid_user_dict_text(text));
        ASSERT_TRUE(!cxxime::is_ordinary_candidate_text(text));
    }
    ASSERT_TRUE(!cxxime::is_ordinary_candidate_text(""));
    ASSERT_TRUE(!cxxime::is_ordinary_candidate_text("\xf0\x80\x80\x80"));
}

TEST(OrdinaryCandidate, graphic_components_do_not_turn_symbols_into_text) {
    for (const std::string text : {u8"\u2103\ufe0f", u8"\u2764\ufe0f!", u8"\U0001f600\u200d!",
                                   u8"1\ufe0f\u20e3!", u8"1\u20e3!", u8"\U000e0061"}) {
        ASSERT_TRUE(!cxxime::is_ordinary_candidate_text(text));
        ASSERT_TRUE(cxxime::is_ordinary_candidate_text(u8"说明" + text));
        ASSERT_TRUE(cxxime::is_ordinary_candidate_text("2" + text));
    }
    for (const auto& text : {u8"1\U0001f600", u8"20\u2103\ufe0f", u8"1\ufe0f"}) {
        ASSERT_TRUE(cxxime::is_ordinary_candidate_text(text));
    }
}

TEST(OrdinaryCandidate, all_frozen_emoji_forms_are_excluded_without_runtime_catalog) {
    std::ifstream input(std::string(CXXIME_DATA_DIR) +
                        "tools/dict_builder/emoji_classification.json");
    const auto data = nlohmann::json::parse(input);
    std::size_t count = 0;
    for (const auto& group : data["groups"]) {
        for (const auto& sequence : group) {
            const std::string text = from_codepoints(sequence.get<std::string>());
            ASSERT_TRUE(!cxxime::is_ordinary_candidate_text(text)) << sequence;
            ++count;
        }
    }
    ASSERT_TRUE(count > 5000);
}

TEST(OrdinaryCandidate, user_lexicon_load_import_and_mutation_apply_same_policy) {
    for (auto kind : {cxxime::UserDictKind::WUBI, cxxime::UserDictKind::PINYIN}) {
        TempFile file(u8"\u2103\tce\t8\tce\n正常\tzhengchang\t2\tzheng:chang\n");
        cxxime::UserLexicon lexicon(kind);
        ASSERT_TRUE(lexicon.load(file.path));
        ASSERT_EQ(lexicon.entry_count(), 1u);
        ASSERT_TRUE(lexicon.contains_text(u8"正常"));
        ASSERT_TRUE(!lexicon.add_entry(u8"\u2103", "ce", "ce"));
        ASSERT_TRUE(!lexicon.add_entry_and_save(u8"\u2103", "ce", "ce"));
        ASSERT_TRUE(!lexicon.replace_entry(u8"正常", "zhengchang", u8"\u2103", "ce", "ce"));
        ASSERT_TRUE(lexicon.import_file(file.path));
        ASSERT_EQ(lexicon.entry_count(), 1u);
        cxxime::UserDataMergeResult merged;
        ASSERT_TRUE(lexicon.merge_contents_and_save(
            u8"\U0001f600\txiao\t1\txiao\nSDK\tkai\t1\tkai\n", &merged));
        ASSERT_EQ(merged.skipped_count, 1u);
        ASSERT_EQ(merged.imported_count, 1u);
        ASSERT_EQ(lexicon.entry_count(), 2u);
    }
}

TEST(OrdinaryCandidate, preference_snapshots_cannot_restore_removed_symbols) {
    TempFile file(u8"\u2103\tce\tce\t8\t1\tce\n正常\tce\tce\t2\t2\tce\n");
    cxxime::CandidatePreference preference(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(preference.load(file.path));
    auto candidates = preference.preferred_candidates("ce", cxxime::CandidateSource::kPinyin);
    ASSERT_EQ(candidates.size(), 1u);
    ASSERT_EQ(candidates[0].text, u8"正常");
    ASSERT_TRUE(preference.record(candidates[0], "ce"));
    cxxime::UserDataMergeResult merged;
    ASSERT_TRUE(preference.merge_contents_and_save(
        u8"\U0001f600\tce\tce\t8\t3\tce\nSDK\tce\tce\t2\t4\tce\n", &merged));
    ASSERT_EQ(merged.skipped_count, 1u);
    ASSERT_EQ(preference.preferred_candidates("ce", cxxime::CandidateSource::kPinyin).size(), 2u);
}

TEST(OrdinaryCandidate, manual_order_removes_middle_symbol_without_losing_following_text) {
    TempFile file(u8"# zhiyi-candidate-order format=1\n"
                  u8"ce\t正常\tce\tce\t1\nce\t\u2103\tce\tce\t2\n"
                  u8"ce\tSDK\tce\tce\t3\nxx\t\U0001f600\txx\t\t1\n");
    cxxime::ManualCandidateOrder order(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(order.load(file.path));
    const auto entries = order.entries_for("ce");
    ASSERT_EQ(entries.size(), 2u);
    ASSERT_EQ(entries[1].text, "SDK");
    ASSERT_TRUE(order.entries_for("xx").empty());
    ASSERT_TRUE(!order.replace_and_save("ce", {{u8"\u2103", "ce", "ce"}}));
    cxxime::UserDataMergeResult merged;
    ASSERT_TRUE(
        order.merge_contents_and_save(u8"# zhiyi-candidate-order format=1\nce\t\u2103\tce\tce\t1\n"
                                      u8"ce\t保留\tce\tce\t2\n",
                                      &merged));
    ASSERT_EQ(merged.skipped_count, 1u);
    ASSERT_EQ(order.entries_for("ce").size(), 1u);
    ASSERT_EQ(order.entries_for("ce")[0].text, u8"保留");
}

TEST(OrdinaryCandidate, composition_learning_filters_loaded_and_imported_records) {
    TempFile file(u8"\u2103\tce\tce\t8\t1\n正常\tce\tce\t2\t2\textra\n");
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(file.path));
    ASSERT_EQ(service.lookup_candidates("ce", 10).size(), 1u);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue({"SDK", "ce", "ce"}));
    ASSERT_TRUE(service.flush());
    cxxime::UserDataMergeResult merged;
    ASSERT_TRUE(service.merge_contents_and_save(u8"\U0001f600\tce\tce\t3\t3\n保留\tce\tce\t1\t4\n",
                                                &merged));
    ASSERT_EQ(merged.skipped_count, 1u);
    ASSERT_EQ(service.lookup_candidates("ce", 10).size(), 3u);
    ASSERT_TRUE(merged.contents.find("extra") != std::string::npos);
    ASSERT_TRUE(service.freeze_and_stop());
}

RUN_ALL_TESTS()
