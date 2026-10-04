// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <sstream>
#include <string>

#include <cxxime/user_data_merge.h>

#include "support/testutil.h"

TEST(UserDataMerge, user_lexicon_merges_valid_rows_and_skips_invalid_rows) {
    const std::string current = "本机词\tbenjici\t2\tben:ji:ci\n"
                                "同名词\ttongmingci\t3\ttong:ming:ci\n";
    const std::string imported = "同名词\ttongmingci\t8\ttong:ming:ci\n"
                                 "导入词\tdaoruci\t4\tdao:ru:ci\n"
                                 "invalid\n";
    cxxime::UserDataMergeResult result;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(cxxime::LexiconResource::kUserLexicon,
                                                        cxxime::UserDictKind::PINYIN, current,
                                                        imported, &result));
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(2));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(1));
    ASSERT_TRUE(result.contents.find("本机词\tbenjici\t2") != std::string::npos);
    ASSERT_TRUE(result.contents.find("同名词\ttongmingci\t8") != std::string::npos);
    ASSERT_TRUE(result.contents.find("导入词\tdaoruci\t4") != std::string::npos);
    ASSERT_EQ(result.contents, "本机词\tbenjici\t2\tben:ji:ci\n"
                               "同名词\ttongmingci\t8\ttong:ming:ci\n"
                               "导入词\tdaoruci\t4\tdao:ru:ci\n");
}

TEST(UserDataMerge, learning_merge_uses_maximum_values_and_is_idempotent) {
    const std::string current = "你好\tnihao\tnihao\t5\t9\tni:hao\n";
    const std::string imported = "你好\tnihao\tnihao\t3\t12\tni:hao\n";
    cxxime::UserDataMergeResult first;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
        cxxime::LexiconResource::kCandidatePreference, cxxime::UserDictKind::PINYIN, current,
        imported, &first));
    ASSERT_TRUE(first.contents.find("你好\tnihao\tnihao\t5\t12\tni:hao") != std::string::npos);

    cxxime::UserDataMergeResult second;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
        cxxime::LexiconResource::kCandidatePreference, cxxime::UserDictKind::PINYIN,
        first.contents, imported, &second));
    ASSERT_EQ(second.contents, first.contents);
}

TEST(UserDataMerge, wubi_learning_and_manual_order_keep_distinct_length_rules) {
    cxxime::UserDataMergeResult learning;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
    cxxime::LexiconResource::kCandidatePreference, cxxime::UserDictKind::WUBI, {},
        "word\tabcd\tabcde\t1\t2\t\n", &learning));
    ASSERT_EQ(learning.imported_count, static_cast<std::size_t>(1));
    ASSERT_EQ(learning.skipped_count, static_cast<std::size_t>(0));

    cxxime::UserDataMergeResult manual_order;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
        cxxime::LexiconResource::kManualCandidateOrder, cxxime::UserDictKind::WUBI, {},
        "# zhiyi-candidate-order format=1\nabcde\tword\tabcd\t\t1\n", &manual_order));
    ASSERT_EQ(manual_order.imported_count, static_cast<std::size_t>(0));
    ASSERT_EQ(manual_order.skipped_count, static_cast<std::size_t>(1));
}

TEST(UserDataMerge, candidate_order_replaces_each_valid_code_group) {
    const std::string header = "# zhiyi-candidate-order format=1\n";
    const std::string current = header + "nihao\t本机首选\tnihao\tni:hao\t1\n"
                                         "zaijian\t再见\tzaijian\tzai:jian\t1\n";
    const std::string imported = header + "nihao\t导入首选\tnihao\tni:hao\t1\n"
                                          "nihao\t导入次选\tnihao\tni:hao\t2\n"
                                          "broken\trow\n";
    cxxime::UserDataMergeResult result;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
        cxxime::LexiconResource::kManualCandidateOrder, cxxime::UserDictKind::PINYIN, current,
        imported, &result));
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(2));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(1));
    ASSERT_TRUE(result.contents.find("本机首选") == std::string::npos);
    ASSERT_TRUE(result.contents.find("导入首选") != std::string::npos);
    ASSERT_TRUE(result.contents.find("zaijian\t再见") != std::string::npos);
}

TEST(UserDataMerge, disabled_words_are_unioned) {
    cxxime::UserDataMergeResult result;
    ASSERT_TRUE(cxxime::merge_lexicon_resource_contents(
        cxxime::LexiconResource::kDisabledSystemLexicon, cxxime::UserDictKind::PINYIN,
        "本机停用\n", "导入停用\n本机停用\n", &result));
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(2));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(0));
    ASSERT_TRUE(result.contents.find("本机停用\n") != std::string::npos);
    ASSERT_TRUE(result.contents.find("导入停用\n") != std::string::npos);
}

TEST(UserDataMerge, composition_learning_applies_runtime_record_limit) {
    std::ostringstream imported;
    for (std::size_t index = 0; index <= 1024; ++index) {
        imported << "学习词" << index << "\tnihao\tni:hao\t1\t" << index + 1 << '\n';
    }
    cxxime::UserDataMergeResult result;
    ASSERT_TRUE(cxxime::merge_composition_learning_contents("", imported.str(), &result));
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(1024));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(1));
    ASSERT_TRUE(result.contents.find("学习词0\t") == std::string::npos);
    ASSERT_TRUE(result.contents.find("学习词1024\t") != std::string::npos);
}
