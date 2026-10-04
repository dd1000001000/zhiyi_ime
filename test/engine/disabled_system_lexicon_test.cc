// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include <cxxime/dict.h>
#include <cxxime/short_code_cache.h>
#include <cxxime/spellings_index.h>
#include <cxxime/syllabifier.h>
#include <cxxime/translator.h>
#include <cxxime/wubi_translator.h>

#include "support/testutil.h"
#include "support/topn_test_data.h"

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

bool contains_text(const std::vector<cxxime::Candidate>& candidates, const std::string& text) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const auto& candidate) { return candidate.text == text; });
}

bool contains_text(const cxxime::CandidatePage& page, const std::string& text) {
    return contains_text(page.candidates, text);
}

cxxime::Candidate
make_candidate(const std::string& text, cxxime::CandidateOrigin origin,
               cxxime::CandidateSource source = cxxime::CandidateSource::kPinyin) {
    cxxime::Candidate candidate;
    candidate.text = text;
    candidate.code = "test";
    candidate.frequency = 100;
    candidate.origin = origin;
    candidate.source = source;
    return candidate;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool temporary_file_exists(const std::string& path) {
    WIN32_FIND_DATAA data = {};
    HANDLE find = FindFirstFileA((path + ".tmp.*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(find);
    return true;
}

} // namespace

TEST(DisabledSystemLexicon, persists_current_format_and_filters_only_system_sources) {
    const std::string path = make_temp_path("dsl");
    ASSERT_TRUE(!path.empty());

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_disabled_system_entries(path));
    ASSERT_TRUE(dictionary.disable_system_entry("停用词"));
    ASSERT_TRUE(dictionary.disable_system_entry("另一个"));
    ASSERT_TRUE(dictionary.save_disabled_system_entries());
    ASSERT_EQ(dictionary.disabled_system_entry_count(), static_cast<std::size_t>(2));

    std::vector<cxxime::Candidate> candidates = {
        make_candidate("停用词", cxxime::CandidateOrigin::kSystem),
        make_candidate("停用词", cxxime::CandidateOrigin::kCache),
        make_candidate("停用词", cxxime::CandidateOrigin::kLearned),
        make_candidate("停用词", cxxime::CandidateOrigin::kComposed),
        make_candidate("停用词", cxxime::CandidateOrigin::kUser),
        make_candidate("停用词", cxxime::CandidateOrigin::kSystem,
                       cxxime::CandidateSource::kSymbol),
        make_candidate("保留词", cxxime::CandidateOrigin::kSystem),
    };
    dictionary.filter_disabled_system_candidates(candidates);
    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(3));
    ASSERT_EQ(candidates[0].origin, cxxime::CandidateOrigin::kUser);
    ASSERT_EQ(candidates[1].source, cxxime::CandidateSource::kSymbol);
    ASSERT_EQ(candidates[2].text, "保留词");

    cxxime::Dict reloaded{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(reloaded.load_disabled_system_entries(path));
    ASSERT_EQ(reloaded.disabled_system_entry_count(), static_cast<std::size_t>(2));
    const auto entries = reloaded.query_disabled_system_entries("停用", 0, 10);
    ASSERT_EQ(entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(entries[0].text, "停用词");
    ASSERT_TRUE(reloaded.restore_system_entry("停用词"));
    ASSERT_TRUE(reloaded.save_disabled_system_entries());

    cxxime::Dict restored{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(restored.load_disabled_system_entries(path));
    ASSERT_TRUE(!restored.is_system_entry_disabled("停用词"));
    ASSERT_TRUE(restored.is_system_entry_disabled("另一个"));
    DeleteFileA(path.c_str());
}

TEST(DisabledSystemLexicon, disabled_system_word_does_not_hide_same_text_user_entry) {
    const std::string dictionary_path = make_temp_path("dsd");
    const std::string user_path = make_temp_path("dsu");
    const std::string disabled_path = make_temp_path("dsl");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dictionary_path, {{"aa", "系统同词", 1000}, {"aa", "系统可见", 900}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open(dictionary_path, user_path));
    ASSERT_TRUE(dictionary.load_disabled_system_entries(disabled_path));
    ASSERT_TRUE(dictionary.disable_system_entry("系统同词"));

    const auto without_user = dictionary.lookup("aa", 10);
    ASSERT_TRUE(!contains_text(without_user, "系统同词"));
    ASSERT_TRUE(contains_text(without_user, "系统可见"));

    ASSERT_TRUE(dictionary.add_user_entry("系统同词", "aa"));
    const auto with_user = dictionary.lookup("aa", 10);
    const auto user = std::find_if(with_user.begin(), with_user.end(), [](const auto& candidate) {
        return candidate.text == "系统同词";
    });
    ASSERT_TRUE(user != with_user.end());
    ASSERT_EQ(user->origin, cxxime::CandidateOrigin::kUser);

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(user_path.c_str());
    DeleteFileA(disabled_path.c_str());
}

TEST(DisabledSystemLexicon, failed_atomic_replace_preserves_previous_file) {
    const std::string path = make_temp_path("dsl");
    ASSERT_TRUE(!path.empty());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "原有\n";
    }

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_disabled_system_entries(path));
    ASSERT_TRUE(dictionary.disable_system_entry("新增"));
    ASSERT_TRUE(SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    const bool saved = dictionary.save_disabled_system_entries();
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    ASSERT_TRUE(!saved);
    ASSERT_EQ(read_file(path), std::string("原有\n"));
    ASSERT_TRUE(!temporary_file_exists(path));
    ASSERT_TRUE(dictionary.save_disabled_system_entries());
    ASSERT_EQ(read_file(path), std::string("原有\n新增\n"));
    DeleteFileA(path.c_str());
}

TEST(DisabledSystemLexicon, failed_transaction_preserves_live_state) {
    const std::string path = make_temp_path("dst");
    ASSERT_TRUE(!path.empty());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "原有\n";
    }

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.load_disabled_system_entries(path));
    ASSERT_TRUE(SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    ASSERT_TRUE(!dictionary.disable_system_entry_and_save("新增"));
    ASSERT_TRUE(!dictionary.is_system_entry_disabled("新增"));
    ASSERT_TRUE(dictionary.is_system_entry_disabled("原有"));
    ASSERT_TRUE(!dictionary.restore_system_entry_and_save("原有"));
    ASSERT_TRUE(dictionary.is_system_entry_disabled("原有"));
    ASSERT_TRUE(SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);

    ASSERT_TRUE(dictionary.disable_system_entry_and_save("新增"));
    ASSERT_TRUE(dictionary.is_system_entry_disabled("新增"));
    ASSERT_TRUE(dictionary.restore_system_entry_and_save("原有"));
    ASSERT_TRUE(!dictionary.is_system_entry_disabled("原有"));
    DeleteFileA(path.c_str());
}

TEST(DisabledSystemLexicon, pinyin_cache_and_learned_fallback_cannot_restore_disabled_word) {
    const std::string dictionary_path = make_temp_path("dsd");
    const std::string disabled_path = make_temp_path("dsl");
    const std::string topn_path = make_temp_path("dst");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dictionary_path,
        {{"ni", "字典候选", 100}, {"ni", "缓存停用", 100}, {"ni", "缓存可见", 100}}));
    ASSERT_TRUE(cxxime::test::create_test_topn(
        topn_path, dictionary_path, {{"ni",
            {make_candidate("缓存停用", cxxime::CandidateOrigin::kSystem),
             make_candidate("缓存可见", cxxime::CandidateOrigin::kSystem)}}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    ASSERT_TRUE(dictionary.load_disabled_system_entries(disabled_path));
    ASSERT_TRUE(dictionary.disable_system_entry("缓存停用"));
    ASSERT_TRUE(dictionary.disable_system_entry("学习停用"));
    ASSERT_TRUE(dictionary.record_candidate_preference(
        make_candidate("学习停用", cxxime::CandidateOrigin::kSystem), "ni"));

    cxxime::ShortCodeCache cache;
    ASSERT_TRUE(cache.load(topn_path, dictionary.candidate_store()));
    cxxime::PinyinTranslator translator;
    translator.set_dict(&dictionary);
    translator.set_short_cache(&cache);
    translator.set_candidate_learning_enabled(true);
    const auto page = translator.translate_page("ni", 0, 10);
    ASSERT_TRUE(!contains_text(page, "缓存停用"));
    ASSERT_TRUE(!contains_text(page, "学习停用"));
    ASSERT_TRUE(contains_text(page, "缓存可见"));

    cache.unload();
    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(disabled_path.c_str());
    DeleteFileA(topn_path.c_str());
}

TEST(DisabledSystemLexicon, composed_candidate_is_filtered_and_restore_invalidates_cache) {
    const std::string dictionary_path = make_temp_path("dsd");
    const std::string disabled_path = make_temp_path("dsl");
    const std::string spellings_path = make_temp_path("dss");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dictionary_path, {{"wu", "无", 9000}, {"shu:chu", "输出", 8000}, {"chu", "出", 6000}}));
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(
        spellings_path, {{"wu", "wu", cxxime::kNormalSpelling, 0.0f},
                         {"shu", "shu", cxxime::kNormalSpelling, 0.0f},
                         {"chu", "chu", cxxime::kNormalSpelling, 0.0f}}));

    cxxime::Dict dictionary{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dictionary.open_dict(dictionary_path));
    ASSERT_TRUE(dictionary.load_disabled_system_entries(disabled_path));
    auto pinyin_resources = cxxime::PinyinResourceSet::create(
        "full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin, spellings_path);
    ASSERT_TRUE(pinyin_resources != nullptr);
    cxxime::PinyinTranslator translator;
    translator.set_dict(&dictionary);
    translator.bind_pinyin(pinyin_resources, {});

    const auto baseline = translator.translate_page("wushuchu", 0, 10);
    ASSERT_TRUE(contains_text(baseline, "无输出"));
    ASSERT_TRUE(dictionary.disable_system_entry("输出"));
    const auto component_disabled = translator.translate_page("wushuchu", 0, 10);
    ASSERT_TRUE(contains_text(component_disabled, "无输出"));
    ASSERT_TRUE(dictionary.restore_system_entry("输出"));
    ASSERT_TRUE(dictionary.disable_system_entry("无输出"));
    const auto disabled = translator.translate_page("wushuchu", 0, 10);
    ASSERT_TRUE(!contains_text(disabled, "无输出"));
    ASSERT_TRUE(dictionary.restore_system_entry("无输出"));
    const auto restored = translator.translate_page("wushuchu", 0, 10);
    ASSERT_TRUE(contains_text(restored, "无输出"));

    dictionary.close();
    DeleteFileA(dictionary_path.c_str());
    DeleteFileA(disabled_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

RUN_ALL_TESTS()
