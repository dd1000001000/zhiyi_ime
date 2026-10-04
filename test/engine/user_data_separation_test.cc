// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/dict.h>
#include <cxxime/engine.h>
#include <cxxime/input_limits.h>
#include <cxxime/key_event.h>
#include <cxxime/spellings_index.h>
#include <cxxime/syllabifier.h>
#include <cxxime/translator.h>
#include <cxxime/user_lexicon.h>
#include <cxxime/wubi_translator.h>

#include "support/testutil.h"
#include "support/test_runtime.h"

namespace {

std::string make_temp_path(const char* prefix) {
    char directory[MAX_PATH] = {};
    char path[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, directory) == 0 ||
        GetTempFileNameA(directory, prefix, 0, path) == 0) {
        return {};
    }
    return path;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

cxxime::Candidate make_candidate(const std::string& text, const std::string& code,
                                 int frequency = 100) {
    cxxime::Candidate candidate;
    candidate.text = text;
    candidate.code = code;
    candidate.frequency = frequency;
    candidate.source = cxxime::CandidateSource::kPinyin;
    return candidate;
}

int candidate_index(const cxxime::Engine& engine, cxxime::CandidateSource source) {
    const auto candidates = engine.context().candidate_page().candidates;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].source == source) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST(UserDataSeparation, manual_entries_and_preferences_use_independent_files) {
    const std::string dictionary_path = make_temp_path("uds");
    const std::string user_path = make_temp_path("udl");
    const std::string preference_path = make_temp_path("udp");
    ASSERT_TRUE(!dictionary_path.empty());
    ASSERT_TRUE(!user_path.empty());
    ASSERT_TRUE(!preference_path.empty());
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, {{"ni:hao", "你好", 1000}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open(dictionary_path, user_path));
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_TRUE(dictionary.add_user_entry("您好", "ninhao", "nin:hao"));
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("你好", "nihao"), "nihao"));
    ASSERT_TRUE(dictionary.save_user_dict());
    ASSERT_TRUE(dictionary.save_candidate_preferences());

    const std::string user_contents = read_file(user_path);
    const std::string preference_contents = read_file(preference_path);
    ASSERT_TRUE(user_contents.find("您好\tninhao") != std::string::npos);
    ASSERT_TRUE(user_contents.find("你好\tnihao") == std::string::npos);
    ASSERT_TRUE(preference_contents.find("你好\tnihao\tnihao\t1\t1\t") != std::string::npos);
    ASSERT_TRUE(preference_contents.find("您好\tninhao") == std::string::npos);
    dictionary.close();

    cxxime::Dict reloaded{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(reloaded.open(dictionary_path, user_path));
    ASSERT_TRUE(reloaded.load_candidate_preferences(preference_path));
    ASSERT_EQ(reloaded.user_entry_count(), static_cast<std::size_t>(1));
    ASSERT_EQ(reloaded.candidate_preference_count(), static_cast<std::size_t>(1));
    reloaded.close();

    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(user_path.c_str());
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, transient_read_failure_preserves_loaded_data) {
    const std::string dictionary_path = make_temp_path("udf");
    const std::string user_path = make_temp_path("udr");
    const std::string preference_path = make_temp_path("udp");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, {{"ni", "你", 1000}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open(dictionary_path, user_path));
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_TRUE(dictionary.add_user_entry("您好", "nin", "nin"));
    ASSERT_TRUE(dictionary.save_user_dict());
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("你", "ni"), "ni"));
    ASSERT_TRUE(dictionary.save_candidate_preferences());

    HANDLE user_lock = CreateFileA(user_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(user_lock != INVALID_HANDLE_VALUE);
    ASSERT_TRUE(!dictionary.load_user_dict(user_path));
    ASSERT_EQ(dictionary.user_entry_count(), static_cast<std::size_t>(1));
    ASSERT_TRUE(dictionary.add_user_entry("你好", "nihao", "ni:hao"));
    ASSERT_TRUE(!dictionary.save_user_dict());
    CloseHandle(user_lock);
    ASSERT_TRUE(dictionary.save_user_dict());
    ASSERT_TRUE(read_file(user_path).find("您好\tnin") != std::string::npos);
    ASSERT_TRUE(read_file(user_path).find("你好\tnihao") != std::string::npos);

    HANDLE preference_lock = CreateFileA(preference_path.c_str(), GENERIC_READ, 0, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(preference_lock != INVALID_HANDLE_VALUE);
    ASSERT_TRUE(!dictionary.load_candidate_preferences(preference_path));
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(1));
    CloseHandle(preference_lock);

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(user_path.c_str());
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, preference_key_keeps_same_text_under_distinct_codes) {
    const std::string preference_path = make_temp_path("udk");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));

    const cxxime::Candidate candidate = make_candidate("你好", "nihao");
    ASSERT_TRUE(dictionary.record_candidate_preference(candidate, "nihao"));
    ASSERT_TRUE(dictionary.record_candidate_preference(candidate, "nh"));
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(2));

    const auto entries = dictionary.query_candidate_preferences("你好", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(2));
    ASSERT_TRUE(entries[0].code != entries[1].code);
    dictionary.close();
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, exact_text_query_does_not_return_prefix_or_code_matches) {
    const std::string user_path = make_temp_path("ude");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));
    ASSERT_TRUE(dictionary.add_user_entry("nihao", "ceshi"));
    ASSERT_TRUE(dictionary.add_user_entry("nihao-long", "shuru"));
    ASSERT_TRUE(dictionary.add_user_entry("other", "nihao"));

    std::size_t total = 0;
    const auto entries = dictionary.query_user_entries("nihao", 0, 16, &total, true);
    ASSERT_EQ(total, static_cast<std::size_t>(1));
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries.front().text, "nihao");
    ASSERT_EQ(entries.front().code, "ceshi");

    dictionary.close();
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, persisted_add_is_idempotent) {
    const std::string user_path = make_temp_path("udi");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));

    ASSERT_TRUE(dictionary.add_user_entry_and_save("target", "nihao"));
    ASSERT_TRUE(dictionary.add_user_entry_and_save("target", "nihao"));

    std::size_t total = 0;
    const auto entries = dictionary.query_user_entries("target", 0, 16, &total, true);
    ASSERT_EQ(1u, total);
    ASSERT_EQ(1u, entries.size());

    dictionary.close();
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, manual_candidate_order_enforces_kind_code_length) {
    const std::string order_path = make_temp_path("udw");
    cxxime::Dict dictionary{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dictionary.load_manual_candidate_order(order_path));
    ASSERT_TRUE(!dictionary.replace_manual_candidate_order_and_save(
        "abcde", {{"word", "abcd", ""}}));
    ASSERT_TRUE(!dictionary.replace_manual_candidate_order_and_save(
        "abcd", {{"word", "abcde", ""}}));
    ASSERT_TRUE(dictionary.replace_manual_candidate_order_and_save(
        "abcd", {{"word", "abcd", ""}}));

    dictionary.close();
    DeleteFileA(order_path.c_str());
}

TEST(UserDataSeparation, wubi_preference_keeps_valid_query_keys_longer_than_four_codes) {
    const std::string preference_path = make_temp_path("udp");
    cxxime::Dict dictionary{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));

    const cxxime::Candidate candidate = make_candidate("word", "abcd");
    ASSERT_TRUE(dictionary.record_candidate_preference(candidate, "abcde"));
    const auto entries = dictionary.query_candidate_preferences("word", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries[0].code, "abcde");

    dictionary.close();
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, manual_candidate_order_rejects_oversized_file_before_reading) {
    const std::string order_path = make_temp_path("udm");
    HANDLE file = CreateFileA(order_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size = {};
    size.QuadPart = 16LL * 1024 * 1024 + 1;
    ASSERT_TRUE(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(file));
    CloseHandle(file);

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(!dictionary.load_manual_candidate_order(order_path));
    DeleteFileA(order_path.c_str());
}

TEST(UserDataSeparation, manual_candidate_order_is_atomic_and_overrides_learning) {
    const std::string dictionary_path = make_temp_path("udo");
    const std::string preference_path = make_temp_path("udp");
    const std::string order_path = make_temp_path("udm");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dictionary_path, {{"yi:y", "默认词", 1000}, {"yi:y", "固定词", 100}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_TRUE(dictionary.load_manual_candidate_order(order_path));
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("默认词", "yiy"), "yiy"));

    const std::uint64_t version = dictionary.manual_candidate_order_version();
    bool conflict = false;
    ASSERT_TRUE(dictionary.replace_manual_candidate_order_if_version(
        "yiy", {{"固定词", "yiy", "yi:y"}}, version, &conflict));
    ASSERT_TRUE(!conflict);
    ASSERT_TRUE(!dictionary.replace_manual_candidate_order_if_version(
        "yiy", {{"默认词", "yiy", "yi:y"}}, version, &conflict));
    ASSERT_TRUE(conflict);

    std::vector<cxxime::Candidate> candidates = {make_candidate("默认词", "yiy", 1000)};
    dictionary.apply_candidate_preferences("yiy", cxxime::CandidateSource::kPinyin, candidates,
                                           10);
    dictionary.apply_manual_candidate_order("yiy", cxxime::CandidateSource::kPinyin, candidates,
                                            10);
    ASSERT_EQ(candidates.front().text, "固定词");
    ASSERT_TRUE(read_file(order_path).find("yiy\t固定词\tyiy\tyi:y\t1") != std::string::npos);
    ASSERT_TRUE(dictionary.clear_candidate_preferences_for_code_and_save("yiy"));
    ASSERT_TRUE(dictionary.query_candidate_preferences("yiy", 0, 10).empty());

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(preference_path.c_str());
    DeleteFileA(order_path.c_str());
}

TEST(UserDataSeparation, manual_candidate_order_uses_complete_pinyin_identity) {
    const std::string dictionary_path = make_temp_path("udd");
    const std::string order_path = make_temp_path("udi");
    const std::string user_path = make_temp_path("udu");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, {{"xi:an", "系统词", 1000}}));
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    ASSERT_TRUE(dictionary.load_user_dict(user_path));
    ASSERT_TRUE(dictionary.load_manual_candidate_order(order_path));
    ASSERT_TRUE(dictionary.replace_manual_candidate_order_and_save(
        "xian", {{"相同词", "xian", "xi:an"}, {"相同词", "xian", "xian"}}));

    cxxime::Candidate whole = make_candidate("相同词", "xian", 1000);
    whole.syllables = "xian";
    cxxime::Candidate split = make_candidate("相同词", "xian", 100);
    split.syllables = "xi:an";
    std::vector<cxxime::Candidate> candidates = {whole, split};
    dictionary.apply_manual_candidate_order("xian", cxxime::CandidateSource::kPinyin, candidates,
                                            10);

    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(candidates[0].syllables, "xi:an");
    ASSERT_EQ(candidates[1].syllables, "xian");
    ASSERT_TRUE(dictionary.has_manual_candidate_order("xian", "相同词", "xian", "xi:an"));
    ASSERT_TRUE(dictionary.has_manual_candidate_order("xian", "相同词", "xian", "xian"));
    ASSERT_TRUE(dictionary.add_user_entry("用户词", "xian", "xi:an"));
    ASSERT_TRUE(dictionary.can_resolve_manual_candidate({"用户词", "xian", "xi:an"},
                                                        cxxime::CandidateSource::kPinyin));
    ASSERT_TRUE(!dictionary.can_resolve_manual_candidate({"用户词", "xian", "xian"},
                                                         cxxime::CandidateSource::kPinyin));
    ASSERT_TRUE(!dictionary.can_resolve_manual_candidate({"用户词", "xian", ""},
                                                         cxxime::CandidateSource::kPinyin));
    ASSERT_TRUE(dictionary.can_resolve_manual_candidate({"系统词", "xian", "xi:an"},
                                                        cxxime::CandidateSource::kPinyin));
    ASSERT_TRUE(!dictionary.can_resolve_manual_candidate({"系统词", "xian", ""},
                                                         cxxime::CandidateSource::kPinyin));

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(order_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, manual_candidate_order_rejects_stale_version_after_reload) {
    const std::string order_path = make_temp_path("udr");
    const std::vector<cxxime::ManualCandidateOrderEntry> first = {{"first", "code", "code"}};
    const std::vector<cxxime::ManualCandidateOrderEntry> second = {{"second", "code", "code"}};

    cxxime::Dict writer{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(writer.load_manual_candidate_order(order_path));
    ASSERT_TRUE(writer.replace_manual_candidate_order_and_save("code", first));
    const std::uint64_t stale_version = writer.manual_candidate_order_version();
    ASSERT_TRUE(writer.replace_manual_candidate_order_and_save("code", second));
    const std::uint64_t current_version = writer.manual_candidate_order_version();
    ASSERT_NE(stale_version, current_version);

    cxxime::Dict reloaded{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(reloaded.load_manual_candidate_order(order_path));
    ASSERT_EQ(reloaded.manual_candidate_order_version(), current_version);
    bool version_conflict = false;
    ASSERT_TRUE(!reloaded.replace_manual_candidate_order_if_version("code", first, stale_version,
                                                                    &version_conflict));
    ASSERT_TRUE(version_conflict);

    DeleteFileA(order_path.c_str());
}

TEST(UserDataSeparation, user_lexicon_rejects_non_current_column_counts) {
    const std::string user_path = make_temp_path("udf");
    {
        std::ofstream output(user_path, std::ios::binary | std::ios::trunc);
        output << "too-few\tfew\n";
        output << "future\tftr\t8\tfu:ture\textra\n";
        output << "current\tnihao\t7\n";
    }

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));
    ASSERT_EQ(dictionary.user_entry_count(), static_cast<std::size_t>(1));
    const auto entries = dictionary.query_user_entries("", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries[0].text, "current");
    dictionary.close();
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, load_quarantines_noncanonical_pinyin_entries) {
    const std::string user_path = make_temp_path("udm");
    const std::string quarantine_path = user_path + ".noncanonical";
    {
        std::ofstream output(user_path, std::ios::binary | std::ios::trunc);
        output << "canonical\tnihao\t2\tni:hao\n";
        output << "unparsed-old-row\ttoo-few\n";
        output << "legacy-double\tnihk\t3\n";
    }

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));
    ASSERT_EQ(dictionary.user_entry_count(), static_cast<std::size_t>(1));
    ASSERT_EQ(dictionary.query_user_entries("", 0, 10).front().text, "canonical");
    ASSERT_TRUE(read_file(user_path).find("legacy-double") == std::string::npos);
    ASSERT_TRUE(read_file(quarantine_path).find("legacy-double\tnihk\t3") != std::string::npos);
    ASSERT_TRUE(read_file(quarantine_path).find("unparsed-old-row\ttoo-few") != std::string::npos);
    ASSERT_TRUE(cxxime::UserLexicon::validate_contents(read_file(user_path),
                                                       cxxime::UserDictKind::PINYIN));

    dictionary.close();
    DeleteFileA(user_path.c_str());
    DeleteFileA(quarantine_path.c_str());
}

TEST(UserDataSeparation, replacing_code_removes_stale_syllable_indexes) {
    const std::string user_path = make_temp_path("udr");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));
    ASSERT_TRUE(dictionary.add_user_entry("manual-entry", "shurufa", "shu:ru:fa"));

    cxxime::QueryBudget budget;
    cxxime::UserLookupStats stats;
    ASSERT_EQ(dictionary.lookup_user_indexed("srf", 10, budget, nullptr, &stats).size(),
              static_cast<std::size_t>(1));
    ASSERT_TRUE(
        dictionary.replace_user_entry("manual-entry", "shurufa", "manual-entry", "xinbianma"));
    ASSERT_TRUE(!dictionary.delete_user_entries({{"manual-entry", "shurufa"}}));
    ASSERT_EQ(dictionary.lookup_user_indexed("srf", 10, budget, nullptr, &stats).size(),
              static_cast<std::size_t>(0));
    const auto exact = dictionary.lookup_user_exact("xinbianma", 10, budget, nullptr, &stats);
    ASSERT_EQ(exact.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(exact[0].text, "manual-entry");

    ASSERT_TRUE(dictionary.save_user_dict());
    ASSERT_TRUE(read_file(user_path).find("\tshu:ru:fa") == std::string::npos);
    dictionary.close();
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, user_lexicon_rejects_invalid_text_codes_and_syllables) {
    const std::string user_path = make_temp_path("udv");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_user_dict(user_path));

    ASSERT_TRUE(!dictionary.add_user_entry(std::string("\xc3\x28", 2), "nihao"));
    ASSERT_TRUE(!dictionary.add_user_entry("uppercase", "ABC"));
    ASSERT_TRUE(!dictionary.add_user_entry("numeric", "abc1"));
    ASSERT_TRUE(!dictionary.add_user_entry("space", "ab c"));
    ASSERT_TRUE(!dictionary.add_user_entry("bad-syllables", "xian", "xi::an"));
    ASSERT_TRUE(dictionary.add_user_entry("valid-entry", "xian", "xi:an"));
    ASSERT_EQ(dictionary.user_entry_count(), static_cast<std::size_t>(1));

    dictionary.close();
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, candidate_preference_rejects_non_current_column_counts) {
    const std::string preference_path = make_temp_path("udp");
    {
        std::ofstream output(preference_path, std::ios::binary | std::ios::trunc);
        output << "too-few\tfew\tfew\t1\t1\n";
        output << "future\tftr\tftr\t3\t2\t\textra\n";
        output << "current\tcur\tcur\t2\t1\t\n";
    }

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(1));
    const auto entries = dictionary.query_candidate_preferences("", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries[0].text, "current");
    dictionary.close();
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, frozen_preferences_reject_all_mutations_after_final_save) {
    const std::string preference_path = make_temp_path("udf");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("first", "first"), "first"));
    ASSERT_TRUE(dictionary.save_candidate_preferences());

    dictionary.freeze_candidate_preferences();
    ASSERT_TRUE(
        !dictionary.record_candidate_preference(make_candidate("second", "second"), "second"));
    ASSERT_TRUE(!dictionary.delete_candidate_preferences({{"first", "first"}}));
    ASSERT_TRUE(!dictionary.clear_candidate_preferences());
    ASSERT_TRUE(dictionary.save_candidate_preferences());
    dictionary.close();

    cxxime::Dict reloaded{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(reloaded.load_candidate_preferences(preference_path));
    ASSERT_EQ(reloaded.candidate_preference_count(), static_cast<std::size_t>(1));
    const auto entries = reloaded.query_candidate_preferences("", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries[0].text, "first");
    reloaded.close();
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, preference_reorders_without_duplicating_and_clear_restores_order) {
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    std::vector<cxxime::Candidate> candidates = {
        make_candidate("默认", "ni", 300),
        make_candidate("偏好", "ni", 200),
    };
    ASSERT_TRUE(dictionary.record_candidate_preference(candidates[1], "ni"));
    dictionary.apply_candidate_preferences("ni", cxxime::CandidateSource::kPinyin, candidates, 10);
    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(candidates[0].text, "偏好");

    ASSERT_TRUE(dictionary.clear_candidate_preferences());
    candidates = {
        make_candidate("默认", "ni", 300),
        make_candidate("偏好", "ni", 200),
    };
    dictionary.apply_candidate_preferences("ni", cxxime::CandidateSource::kPinyin, candidates, 10);
    ASSERT_EQ(candidates[0].text, "默认");
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(0));
}

TEST(UserDataSeparation, failed_preference_transaction_preserves_live_state) {
    const std::string preference_path = make_temp_path("udt");
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_candidate_preferences(preference_path));
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("保留", "baoliu"), "baoliu"));
    ASSERT_TRUE(dictionary.record_candidate_preference(make_candidate("保留二", "baoliuer"),
                                                       "baoliuer"));
    ASSERT_TRUE(dictionary.save_candidate_preferences());
    ASSERT_TRUE(DeleteFileA(preference_path.c_str()) != FALSE);
    ASSERT_TRUE(CreateDirectoryA(preference_path.c_str(), nullptr) != FALSE);

    const std::vector<cxxime::LexiconEntryKey> entries = {
        {"保留", "baoliu"}, {"保留二", "baoliuer"}};
    ASSERT_TRUE(!dictionary.delete_candidate_preferences_and_save(entries));
    ASSERT_EQ(dictionary.query_candidate_preferences("保留", 0, 10).size(),
              static_cast<std::size_t>(2));
    ASSERT_TRUE(!dictionary.clear_candidate_preferences_and_save());
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(2));

    ASSERT_TRUE(RemoveDirectoryA(preference_path.c_str()) != FALSE);
    const std::uint64_t version_before_delete = dictionary.candidate_preference_version();
    ASSERT_TRUE(dictionary.delete_candidate_preferences_and_save(entries));
    ASSERT_EQ(dictionary.candidate_preference_version(), version_before_delete + 1);
    ASSERT_TRUE(dictionary.query_candidate_preferences("保留", 0, 10).empty());
    ASSERT_TRUE(
        dictionary.record_candidate_preference(make_candidate("清空", "qingkong"), "qingkong"));
    ASSERT_TRUE(dictionary.clear_candidate_preferences_and_save());
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(0));
    DeleteFileA(preference_path.c_str());
}

TEST(UserDataSeparation, missing_preference_is_only_a_low_priority_fallback) {
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    const cxxime::Candidate learned = make_candidate("曾用候选", "ni", 200);
    ASSERT_TRUE(dictionary.record_candidate_preference(learned, "ni"));

    std::vector<cxxime::Candidate> candidates = {make_candidate("当前候选", "ni", 100)};
    dictionary.apply_candidate_preferences("ni", cxxime::CandidateSource::kPinyin, candidates, 10);
    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(candidates[0].text, "当前候选");
    ASSERT_EQ(candidates[1].text, "曾用候选");
    ASSERT_EQ(candidates[1].origin, cxxime::CandidateOrigin::kLearned);
    dictionary.apply_candidate_preferences("ni", cxxime::CandidateSource::kPinyin, candidates, 10);
    ASSERT_EQ(candidates[0].text, "当前候选");
    ASSERT_EQ(candidates[1].text, "曾用候选");

    candidates = {make_candidate("当前候选", "ni", 100)};
    dictionary.apply_candidate_preferences("ni", cxxime::CandidateSource::kPinyin, candidates, 1);
    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(candidates[0].text, "当前候选");
}

TEST(UserDataSeparation, deep_candidate_preference_is_applied_before_pagination) {
    const std::string dictionary_path = make_temp_path("udp");
    std::vector<std::tuple<std::string, std::string, int>> entries;
    for (int index = 0; index < 20; ++index) {
        entries.push_back({"abcd", "candidate-" + std::to_string(index), 1000 - index});
    }
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, entries));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    auto unranked = dictionary.lookup("abcd", 20);
    ASSERT_EQ(unranked.size(), static_cast<std::size_t>(20));
    cxxime::Candidate preferred = unranked[14];
    preferred.source = cxxime::CandidateSource::kWubi;
    ASSERT_TRUE(dictionary.record_candidate_preference(preferred, "abcd"));

    cxxime::WubiTranslator translator;
    translator.set_dict(&dictionary);
    translator.set_candidate_learning_enabled(true);
    const auto first_page = translator.translate_page("abcd", 0, 5);
    const auto second_page = translator.translate_page("abcd", 1, 5);
    ASSERT_EQ(first_page.candidates[0].text, preferred.text);
    ASSERT_TRUE(std::none_of(
        second_page.candidates.begin(), second_page.candidates.end(),
        [&](const cxxime::Candidate& candidate) { return candidate.text == preferred.text; }));

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
}

TEST(UserDataSeparation, deep_manual_prefix_preference_uses_candidate_full_code) {
    const std::string dictionary_path = make_temp_path("udp");
    const std::string user_path = make_temp_path("udl");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, {{"zz", "system", 1}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open(dictionary_path, user_path));
    const std::vector<std::string> codes = {"niba",   "nibai",  "niban",  "nibang", "nibao",
                                            "nibei",  "niben",  "nibeng", "nibi",   "nibian",
                                            "nibiao", "nibie",  "nibin",  "nibing", "nibo",
                                            "nibu",   "nida",   "nidai",  "nidan",  "nidang"};
    for (std::size_t index = 0; index < codes.size(); ++index) {
        ASSERT_TRUE(dictionary.add_user_entry("manual-" + std::to_string(index), codes[index]));
    }
    auto unranked = dictionary.lookup("ni", 20);
    ASSERT_EQ(unranked.size(), static_cast<std::size_t>(20));
    cxxime::Candidate preferred = unranked[14];
    preferred.source = cxxime::CandidateSource::kWubi;
    ASSERT_TRUE(dictionary.record_candidate_preference(preferred, "ni"));

    cxxime::WubiTranslator translator;
    translator.set_dict(&dictionary);
    translator.set_candidate_learning_enabled(true);
    const auto first_page = translator.translate_page("ni", 0, 5);
    ASSERT_EQ(first_page.candidates[0].text, preferred.text);

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(UserDataSeparation, deep_pinyin_preference_is_applied_before_pagination) {
    const std::string dictionary_path = make_temp_path("udp");
    const std::string spellings_path = make_temp_path("uds");
    std::vector<std::tuple<std::string, std::string, int>> entries;
    for (int index = 0; index < 20; ++index) {
        entries.push_back({"ni", "pinyin-" + std::to_string(index), 1000 - index});
    }
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dictionary_path, entries));
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {{"ni", "ni", 0, 0.0f}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    auto unranked = dictionary.lookup_by_syllables({"ni"}, 20);
    ASSERT_EQ(unranked.size(), static_cast<std::size_t>(20));
    const cxxime::Candidate preferred = unranked[14];
    ASSERT_TRUE(dictionary.record_candidate_preference(preferred, "ni"));

    auto pinyin_resources = cxxime::PinyinResourceSet::create(
        "full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin, spellings_path);
    ASSERT_TRUE(pinyin_resources != nullptr);
    cxxime::PinyinTranslator translator;
    translator.set_dict(&dictionary);
    translator.bind_pinyin(pinyin_resources, {});
    translator.set_candidate_learning_enabled(true);
    const auto first_page = translator.translate_page("ni", 0, 5);
    const auto second_page = translator.translate_page("ni", 1, 5);
    ASSERT_EQ(first_page.candidates[0].text, preferred.text);
    ASSERT_TRUE(std::none_of(
        second_page.candidates.begin(), second_page.candidates.end(),
        [&](const cxxime::Candidate& candidate) { return candidate.text == preferred.text; }));

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(UserDataSeparation, symbols_and_composed_candidates_are_not_learned) {
    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    cxxime::Candidate symbol = make_candidate("。", "bd");
    symbol.source = cxxime::CandidateSource::kSymbol;
    ASSERT_TRUE(!dictionary.record_candidate_preference(symbol, "bd"));

    cxxime::Candidate composed = make_candidate("你好世界", "nihaoshijie");
    composed.origin = cxxime::CandidateOrigin::kComposed;
    ASSERT_TRUE(!dictionary.record_candidate_preference(composed, "nihaoshijie"));
    ASSERT_EQ(dictionary.candidate_preference_count(), static_cast<std::size_t>(0));
}

RUN_ALL_TESTS()
