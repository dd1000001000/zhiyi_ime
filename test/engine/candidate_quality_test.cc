// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "support/testutil.h"

#include <windows.h>
#include <json.hpp>
#include <cxxime/dict.h>
#include <cxxime/context.h>
#include <cxxime/mixed_translator.h>
#include <cxxime/processor.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_scratch.h>
#include <cxxime/query_trace.h>
#include <cxxime/spellings_index.h>
#include <cxxime/syllabifier.h>
#include <cxxime/translator.h>
#include <cxxime/wubi_translator.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using json = nlohmann::json;

struct ExpectedCandidate {
    std::string text;
    int max_rank = 0;  // 1-based, 0 means any visible rank.
};

struct UserEntrySetup {
    std::string mode;
    std::string text;
    std::string code;
    std::string syllables;
};

struct QualityCase {
    std::string id;
    std::string mode;
    std::string input;
    int page_size = 9;
    int min_candidates = 0;
    int max_elapsed_us = 0;
    bool require_unique = true;
    bool has_cache_requirement = false;
    bool require_cache_hit = false;
    std::vector<ExpectedCandidate> contains;
    std::vector<std::string> forbid_top1;
    std::vector<std::string> forbid_visible;
    std::unordered_map<std::string, std::string> expect_sources;
    std::unordered_map<std::string, std::string> expect_origins;
    std::vector<UserEntrySetup> setup_user_entries;
};

std::string project_path(const char* rel) {
    return std::string(CXXIME_PROJECT_DIR) + rel;
}

std::string temp_path(const char* filename) {
    char dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + filename;
}

std::string source_name(cxxime::CandidateSource source) {
    switch (source) {
    case cxxime::CandidateSource::kPinyin:
        return "pinyin";
    case cxxime::CandidateSource::kWubi:
        return "wubi";
    case cxxime::CandidateSource::kSymbol:
        return "symbol";
    case cxxime::CandidateSource::kEnglish:
        return "english";
    }
    return "unknown";
}

std::string origin_name(cxxime::CandidateOrigin origin) {
    switch (origin) {
    case cxxime::CandidateOrigin::kUser:
        return "user";
    case cxxime::CandidateOrigin::kLearned:
        return "learned";
    case cxxime::CandidateOrigin::kCache:
        return "cache";
    case cxxime::CandidateOrigin::kComposed:
        return "composed";
    case cxxime::CandidateOrigin::kSystem:
    default:
        return "system";
    }
}

std::string candidate_summary(const cxxime::CandidatePage& page) {
    std::string out;
    for (size_t i = 0; i < page.candidates.size(); ++i) {
        const auto& c = page.candidates[i];
        if (!out.empty())
            out += " | ";
        out += std::to_string(i + 1);
        out += ".";
        out += c.text;
        out += "/";
        out += source_name(c.source);
        out += "/";
        out += origin_name(c.origin);
        out += "/";
        out += std::to_string(c.frequency);
    }
    return out;
}

int rank_of(const cxxime::CandidatePage& page, const std::string& text) {
    for (size_t i = 0; i < page.candidates.size(); ++i) {
        if (page.candidates[i].text == text)
            return static_cast<int>(i) + 1;
    }
    return 0;
}

const cxxime::Candidate* find_candidate(const cxxime::CandidatePage& page,
                                         const std::string& text) {
    for (const auto& c : page.candidates) {
        if (c.text == text)
            return &c;
    }
    return nullptr;
}

ExpectedCandidate parse_expected_candidate(const json& value) {
    ExpectedCandidate expected;
    if (value.is_string()) {
        expected.text = value.get<std::string>();
    } else {
        expected.text = value.at("text").get<std::string>();
        if (value.contains("max_rank"))
            expected.max_rank = value.at("max_rank").get<int>();
    }
    return expected;
}

UserEntrySetup parse_user_entry_setup(const json& value, const std::string& default_mode) {
    UserEntrySetup setup;
    setup.mode = value.value("mode", default_mode);
    setup.text = value.at("text").get<std::string>();
    setup.code = value.at("code").get<std::string>();
    setup.syllables = value.value("syllables", std::string{});
    return setup;
}

QualityCase parse_case(const json& item, const json& defaults) {
    QualityCase q;
    q.id = item.at("id").get<std::string>();
    q.mode = item.at("mode").get<std::string>();
    q.input = item.at("input").get<std::string>();

    auto get_int = [&](const char* key, int fallback) {
        if (item.contains(key))
            return item.at(key).get<int>();
        if (defaults.contains(key))
            return defaults.at(key).get<int>();
        return fallback;
    };
    auto get_bool = [&](const char* key, bool fallback) {
        if (item.contains(key))
            return item.at(key).get<bool>();
        if (defaults.contains(key))
            return defaults.at(key).get<bool>();
        return fallback;
    };

    q.page_size = get_int("page_size", 9);
    q.min_candidates = get_int("min_candidates", 0);
    q.max_elapsed_us = get_int("max_elapsed_us", 0);
    q.require_unique = get_bool("require_unique", true);

    if (item.contains("require_cache_hit")) {
        q.has_cache_requirement = true;
        q.require_cache_hit = item.at("require_cache_hit").get<bool>();
    }

    if (item.contains("contains")) {
        for (const auto& expected : item.at("contains"))
            q.contains.push_back(parse_expected_candidate(expected));
    }
    if (item.contains("forbid_top1")) {
        for (const auto& text : item.at("forbid_top1"))
            q.forbid_top1.push_back(text.get<std::string>());
    }
    if (item.contains("forbid_visible")) {
        for (const auto& text : item.at("forbid_visible"))
            q.forbid_visible.push_back(text.get<std::string>());
    }
    if (item.contains("expect_sources")) {
        for (auto it = item.at("expect_sources").begin();
             it != item.at("expect_sources").end(); ++it)
            q.expect_sources[it.key()] = it.value().get<std::string>();
    }
    if (item.contains("expect_origins")) {
        for (auto it = item.at("expect_origins").begin();
             it != item.at("expect_origins").end(); ++it)
            q.expect_origins[it.key()] = it.value().get<std::string>();
    }
    if (item.contains("setup_user_entries")) {
        for (const auto& setup : item.at("setup_user_entries"))
            q.setup_user_entries.push_back(parse_user_entry_setup(setup, q.mode));
    }
    return q;
}

std::vector<QualityCase> load_cases() {
    std::ifstream file(project_path("test/engine/candidate_quality_cases.json"));
    ASSERT_TRUE(file.is_open());

    json root = json::parse(file);
    ASSERT_EQ(root.at("schema").get<int>(), 1);

    json defaults = root.value("defaults", json::object());
    std::vector<QualityCase> cases;
    for (const auto& item : root.at("cases")) {
        if (item.value("enabled", true))
            cases.push_back(parse_case(item, defaults));
    }
    return cases;
}

class QualityHarness {
public:
    QualityHarness() {
        SetConsoleOutputCP(CP_UTF8);
        DeleteFileA(pinyin_user_path_.c_str());
        DeleteFileA(wubi_user_path_.c_str());

        ASSERT_TRUE(pinyin_dict_.open_bundle(
            project_path("data/pinyin.dict.bin"),
            pinyin_user_path_,
            project_path("data/pinyin.dict.idx"),
            project_path("data/pinyin.topn.bin")));
        ASSERT_TRUE(wubi_dict_.open_wubi_bundle(
            project_path("data/wubi86.dict.bin"),
            wubi_user_path_,
            project_path("data/wubi86.dict.idx")));
        pinyin_resources_ = cxxime::PinyinResourceSet::create(
            "full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin,
            project_path("data/pinyin.spellings.bin"));
        ASSERT_TRUE(pinyin_resources_ != nullptr);

        pinyin_.set_dict(&pinyin_dict_);
        pinyin_.bind_pinyin(pinyin_resources_, {});
        pinyin_.set_short_cache(&pinyin_dict_.short_cache());

        wubi_.set_dict(&wubi_dict_);

        mixed_.set_pinyin_dict(&pinyin_dict_);
        mixed_.set_wubi_dict(&wubi_dict_);
        mixed_.bind_pinyin(pinyin_resources_, {});
        mixed_.set_short_cache(&pinyin_dict_.short_cache());
    }

    ~QualityHarness() {
        pinyin_dict_.close();
        wubi_dict_.close();
        DeleteFileA(pinyin_user_path_.c_str());
        DeleteFileA(wubi_user_path_.c_str());
    }

    cxxime::CandidatePage translate(const QualityCase& q, cxxime::QueryTrace& trace,
                                     int64_t& elapsed_us) {
        cxxime::QueryBudget budget = cxxime::make_budget(
            static_cast<int>(q.input.size()), q.page_size);
        cxxime::QueryScratch scratch;

        auto start = std::chrono::steady_clock::now();
        cxxime::CandidatePage page;
        if (q.mode == "pinyin") {
            page = pinyin_.translate_page(q.input, 0, q.page_size, &trace, &budget, &scratch);
        } else if (q.mode == "wubi") {
            page = wubi_.translate_page(q.input, 0, q.page_size, &trace, &budget, &scratch);
        } else if (q.mode == "mixed") {
            page = mixed_.translate_page(q.input, 0, q.page_size, &trace, &budget, &scratch);
        } else {
            ASSERT_TRUE(false) << "unknown quality mode: " << q.mode;
        }
        elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        return page;
    }

    void apply_setup(const QualityCase& q) {
        for (const auto& setup : q.setup_user_entries) {
            auto* dict = dict_for_mode(setup.mode);
            ASSERT_TRUE(dict != nullptr) << "unknown setup mode: " << setup.mode;
            ASSERT_TRUE(dict->add_user_entry(setup.text, setup.code, setup.syllables));
        }
    }

    void cleanup_setup(const QualityCase& q) {
        for (const auto& setup : q.setup_user_entries) {
            auto* dict = dict_for_mode(setup.mode);
            if (dict)
                dict->delete_user_entries({{setup.text, setup.code}});
        }
    }

private:
    cxxime::Dict* dict_for_mode(const std::string& mode) {
        if (mode == "pinyin")
            return &pinyin_dict_;
        if (mode == "wubi")
            return &wubi_dict_;
        return nullptr;
    }

    std::string pinyin_user_path_ = temp_path("zhiyi_quality_pinyin_user.tsv");
    std::string wubi_user_path_ = temp_path("zhiyi_quality_wubi_user.tsv");
    cxxime::Dict pinyin_dict_{cxxime::UserDictKind::PINYIN};
    cxxime::Dict wubi_dict_{cxxime::UserDictKind::WUBI};
    std::shared_ptr<const cxxime::PinyinResourceSet> pinyin_resources_;
    cxxime::PinyinTranslator pinyin_;
    cxxime::WubiTranslator wubi_;
    cxxime::MixedTranslator mixed_;
};

class ScopedUserSetup {
public:
    ScopedUserSetup(QualityHarness& harness, const QualityCase& q)
        : harness_(harness), q_(q) {
        harness_.apply_setup(q_);
    }

    ~ScopedUserSetup() {
        harness_.cleanup_setup(q_);
    }

private:
    QualityHarness& harness_;
    const QualityCase& q_;
};

void assert_quality_case(QualityHarness& harness, const QualityCase& q) {
    ScopedUserSetup setup(harness, q);

    cxxime::QueryTrace trace = {};
    int64_t elapsed_us = 0;
    auto page = harness.translate(q, trace, elapsed_us);
    std::string summary = candidate_summary(page);

    if (q.min_candidates > 0) {
        ASSERT_GE(static_cast<int>(page.candidates.size()), q.min_candidates)
            << q.id << " candidates: " << summary;
    }
    if (!q.contains.empty()) {
        ASSERT_TRUE(!page.candidates.empty()) << q.id << " candidates: " << summary;
    }

    if (q.require_unique) {
        std::set<std::string> seen;
        for (const auto& c : page.candidates) {
            ASSERT_TRUE(seen.insert(c.text).second)
                << q.id << " duplicated candidate: " << c.text
                << " candidates: " << summary;
        }
    }

    for (const auto& expected : q.contains) {
        int rank = rank_of(page, expected.text);
        ASSERT_GT(rank, 0) << q.id << " missing candidate: " << expected.text
            << " candidates: " << summary;
        if (expected.max_rank > 0) {
            ASSERT_LE(rank, expected.max_rank)
                << q.id << " candidate rank too low: " << expected.text
                << " rank=" << rank
                << " max_rank=" << expected.max_rank
                << " candidates: " << summary;
        }
    }

    if (!q.forbid_top1.empty() && !page.candidates.empty()) {
        const auto& top1 = page.candidates.front().text;
        for (const auto& forbidden : q.forbid_top1) {
            ASSERT_NE(top1, forbidden)
                << q.id << " forbidden top1: " << top1
                << " candidates: " << summary;
        }
    }

    for (const auto& forbidden : q.forbid_visible) {
        ASSERT_EQ(rank_of(page, forbidden), 0)
            << q.id << " forbidden visible candidate: " << forbidden
            << " candidates: " << summary;
    }

    for (const auto& [text, expected_source] : q.expect_sources) {
        const auto* candidate = find_candidate(page, text);
        ASSERT_TRUE(candidate != nullptr) << q.id << " missing source target: " << text
            << " candidates: " << summary;
        ASSERT_EQ(source_name(candidate->source), expected_source)
            << q.id << " source mismatch for " << text
            << " candidates: " << summary;
    }

    for (const auto& [text, expected_origin] : q.expect_origins) {
        const auto* candidate = find_candidate(page, text);
        ASSERT_TRUE(candidate != nullptr) << q.id << " missing origin target: " << text
            << " candidates: " << summary;
        ASSERT_EQ(origin_name(candidate->origin), expected_origin)
            << q.id << " origin mismatch for " << text
            << " candidates: " << summary;
    }

    if (q.has_cache_requirement) {
        ASSERT_EQ(trace.cache_hit, q.require_cache_hit)
            << q.id << " cache_hit mismatch candidates: " << summary;
    }
    if (q.max_elapsed_us > 0) {
        ASSERT_LE(elapsed_us, q.max_elapsed_us)
            << q.id << " elapsed_us=" << elapsed_us
            << " candidates: " << summary;
    }
}

} // namespace

TEST(CandidateQuality, golden_cases) {
    auto cases = load_cases();
    ASSERT_TRUE(!cases.empty());

    QualityHarness harness;
    for (const auto& q : cases) {
        std::fprintf(stderr, "  case %s ...", q.id.c_str());
        assert_quality_case(harness, q);
        std::fprintf(stderr, " OK\n");
    }
}

TEST(CandidateQuality, pinyin_tc_candidate_and_space_commit_match) {
    QualityHarness harness;
    QualityCase q;
    q.id = "pinyin-short-tc-top1";
    q.mode = "pinyin";
    q.input = "tc";
    q.page_size = 9;

    cxxime::QueryTrace trace = {};
    int64_t elapsed_us = 0;
    auto page = harness.translate(q, trace, elapsed_us);
    std::string summary = candidate_summary(page);

    ASSERT_TRUE(!page.candidates.empty()) << "tc candidates: " << summary;
    ASSERT_EQ(page.highlighted, 0) << "tc candidates: " << summary;
    ASSERT_EQ(page.candidates.front().text, u8"提出") << "tc candidates: " << summary;

    cxxime::Context context;
    ASSERT_TRUE(context.set_preedit("tc"));
    context.update_candidates(std::move(page));

    cxxime::PinyinProcessor processor;
    cxxime::KeyEvent space;
    space.keycode = VK_SPACE;
    auto result = processor.process_key(space, context);

    ASSERT_EQ(result, cxxime::ProcessResult::CANDIDATE_SELECTED);
    const int selected = context.take_requested_candidate_selection().value_or(-1);
    ASSERT_EQ(selected, 0);
    ASSERT_TRUE(context.commit_candidate(selected));
    ASSERT_EQ(context.committed_text, u8"提出");
}

RUN_ALL_TESTS()
