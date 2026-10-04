// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "wubi_engine_test_support.h"
TEST(WubiEngine, processor_letter_input) {
    cxxime::WubiProcessor proc;
    cxxime::Context ctx;

    // 输入字母 a
    auto r = proc.process_key(make_key('A'), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(ctx.active_input(), "a");

    // 输入字母 b
    r = proc.process_key(make_key('B'), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(ctx.active_input(), "ab");

    // Escape 清空
    r = proc.process_key(make_key(VK_ESCAPE), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(ctx.active_input().empty());
}

TEST(WubiEngine, processor_backspace) {
    cxxime::WubiProcessor proc;
    cxxime::Context ctx;

    proc.process_key(make_key('A'), ctx);
    proc.process_key(make_key('B'), ctx);
    ASSERT_EQ(ctx.active_input(), "ab");

    auto r = proc.process_key(make_key(VK_BACK), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(ctx.active_input(), "a");

    r = proc.process_key(make_key(VK_BACK), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(ctx.active_input().empty());

    // 空输入时按 Backspace，应 REJECTED
    r = proc.process_key(make_key(VK_BACK), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::REJECTED);
}

TEST(WubiEngine, processor_number_select) {
    cxxime::WubiProcessor proc;
    cxxime::Context ctx;

    // 设置候选
    ASSERT_TRUE(ctx.set_preedit("a"));
    cxxime::CandidatePage page;
    cxxime::Candidate c1; c1.text = "工"; c1.frequency = 300;
    cxxime::Candidate c2; c2.text = "式"; c2.frequency = 200;
    page.candidates = {c1, c2};
    page.highlighted = 0;
    ctx.update_candidates(std::move(page));

    // 按数字键 2 选中 "式"
    auto r = proc.process_key(make_key('2'), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::CANDIDATE_SELECTED);
    ASSERT_EQ(ctx.take_requested_candidate_selection().value_or(-1), 1);
}

TEST(WubiEngine, processor_space_select_first) {
    cxxime::WubiProcessor proc;
    cxxime::Context ctx;

    // 设置候选
    ASSERT_TRUE(ctx.set_preedit("a"));
    cxxime::CandidatePage page;
    cxxime::Candidate c1; c1.text = "工"; c1.frequency = 300;
    cxxime::Candidate c2; c2.text = "式"; c2.frequency = 200;
    page.candidates = {c1, c2};
    page.highlighted = 0;
    ctx.update_candidates(std::move(page));

    // Space 选中第一候选
    auto r = proc.process_key(make_key(VK_SPACE), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::CANDIDATE_SELECTED);
    ASSERT_EQ(ctx.take_requested_candidate_selection().value_or(-1), 0);
}

TEST(WubiEngine, processor_space_without_candidates_clears) {
    cxxime::WubiProcessor proc;
    cxxime::Context ctx;

    ASSERT_TRUE(ctx.set_preedit("niwe"));

    auto r = proc.process_key(make_key(VK_SPACE), ctx);
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(ctx.active_input().empty());
    ASSERT_TRUE(ctx.committed_text.empty());
    ASSERT_TRUE(ctx.candidate_page().candidates.empty());
}

// --- WubiTranslator tests ---

TEST(WubiEngine, translator_basic_lookup) {
    std::string dict_path = make_temp_path("test_wubi_trans.bin");
    cxxime::Dict::create_test_dict(dict_path, {
        {"a", "工", 300},
        {"aaaa", "工", 300},
        {"aa", "式", 200},
    });

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open(dict_path));

    cxxime::WubiTranslator trans;
    trans.set_dict(&dict);

    // 查询 "a"，应返回 "工" 和/或 "式"
    auto page = trans.translate_page("a", 0, 9);
    ASSERT_GE(page.candidates.size(), 1u);

    // 查询 "aa"，应返回 "式"
    page = trans.translate_page("aa", 0, 9);
    ASSERT_GE(page.candidates.size(), 1u);
    ASSERT_EQ(page.candidates[0].text, "式");

    dict.close();
    DeleteFileA(dict_path.c_str());
}

TEST(WubiEngine, translator_uses_explicit_candidate_offset) {
    std::string dict_path = make_temp_path("test_wubi_offset.bin");
    const std::vector<std::tuple<std::string, std::string, int>> entries = {
        {"a", "一", 500},  {"aa", "二", 400}, {"ab", "三", 300},
        {"ac", "四", 200}, {"ad", "五", 100},
    };
    cxxime::Dict::create_test_dict(dict_path, entries);

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open(dict_path));

    cxxime::WubiTranslator translator;
    translator.set_dict(&dict);
    auto first = translator.translate_page("a", 0, 2);
    auto second = translator.translate_page("a", 1, 2, nullptr, nullptr, nullptr, 2);

    ASSERT_EQ(first.page_offset, 0);
    ASSERT_EQ(second.page_index, 1);
    ASSERT_EQ(second.page_offset, 2);
    ASSERT_EQ(first.candidates.size(), 2u);
    ASSERT_EQ(second.candidates.size(), 2u);
    for (const auto& first_candidate : first.candidates) {
        for (const auto& second_candidate : second.candidates) {
            ASSERT_TRUE(first_candidate.text != second_candidate.text);
        }
    }

    dict.close();
    DeleteFileA(dict_path.c_str());
}

TEST(WubiEngine, translator_keeps_candidate_order_stable_when_page_query_expands) {
    std::string dict_path = make_temp_path("test_wubi_stable_pages.bin");
    std::string index_path = dict_path + ".idx";
    std::string user_dict_path = make_temp_path("test_wubi_stable_pages_user.tsv");
    DeleteFileA(user_dict_path.c_str());
    const std::vector<std::tuple<std::string, std::string, int>> entries = {
        {"a", "exact", 500},   {"aa", "early", 1},    {"ab", "rank-1", 500},
        {"ac", "rank-2", 600}, {"ad", "rank-3", 700}, {"ae", "rank-4", 800},
    };
    cxxime::Dict::create_test_dict(dict_path, entries);
    ASSERT_TRUE(cxxime::test::create_test_wubi_index(index_path, entries));

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open_wubi_bundle(dict_path, user_dict_path, index_path));

    cxxime::WubiTranslator translator;
    translator.set_dict(&dict);
    auto first = translator.translate_page("a", 0, 2);
    auto second = translator.translate_page("a", 1, 2, nullptr, nullptr, nullptr, 2);

    cxxime::WubiTranslator wide_translator;
    wide_translator.set_dict(&dict);
    auto wide = wide_translator.translate_page("a", 0, 5);

    ASSERT_EQ(first.candidates.size(), 2u);
    ASSERT_EQ(second.candidates.size(), 2u);
    ASSERT_GE(wide.candidates.size(), 4u);
    ASSERT_EQ(first.candidates[0].text, "exact");
    ASSERT_EQ(first.candidates[1].text, "rank-4");
    ASSERT_EQ(second.candidates[0].text, "rank-3");
    ASSERT_EQ(second.candidates[1].text, "rank-2");
    for (size_t index = 0; index < 2; ++index) {
        ASSERT_EQ(first.candidates[index].text, wide.candidates[index].text);
        ASSERT_EQ(second.candidates[index].text, wide.candidates[index + 2].text);
    }

    dict.close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(index_path.c_str());
    DeleteFileA(user_dict_path.c_str());
}

TEST(WubiEngine, clear_query_cache_discards_candidate_snapshot) {
    const std::string dict_path = make_temp_path("test_wubi_clear_query_cache.bin");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dict_path, {{"a", "exact", 500}, {"aa", "prefix", 400}}));

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open(dict_path));

    cxxime::WubiTranslator translator;
    translator.set_dict(&dict);
    cxxime::QueryBudget budget;

    cxxime::QueryTrace initial_trace = {};
    const auto initial = translator.translate_page("a", 0, 9, &initial_trace, &budget);
    ASSERT_EQ(initial.candidates.size(), 2u);
    ASSERT_TRUE(initial_trace.exact_scan_count + initial_trace.prefix_scan_count > 0);

    cxxime::QueryTrace cached_trace = {};
    const auto cached = translator.translate_page("a", 0, 9, &cached_trace, &budget);
    ASSERT_EQ(cached.candidates.size(), initial.candidates.size());
    ASSERT_EQ(cached_trace.exact_scan_count + cached_trace.prefix_scan_count, 0u);

    translator.clear_query_cache();

    cxxime::QueryTrace cleared_trace = {};
    const auto cleared = translator.translate_page("a", 0, 9, &cleared_trace, &budget);
    ASSERT_EQ(cleared.candidates.size(), initial.candidates.size());
    ASSERT_TRUE(cleared_trace.exact_scan_count + cleared_trace.prefix_scan_count > 0);

    dict.close();
    DeleteFileA(dict_path.c_str());
}

TEST(WubiEngine, translator_empty_code) {
    std::string dict_path = make_temp_path("test_wubi_empty.bin");
    cxxime::Dict::create_test_dict(dict_path, {
        {"a", "工", 300},
    });

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open(dict_path));

    cxxime::WubiTranslator trans;
    trans.set_dict(&dict);

    // 空输入应返回空结果
    auto page = trans.translate_page("", 0, 9);
    ASSERT_EQ(page.candidates.size(), 0u);

    dict.close();
    DeleteFileA(dict_path.c_str());
}

int main() {
    GetTempPathA(MAX_PATH, wubi_engine_test_temp_path);
    return test::RunAllTests();
}
