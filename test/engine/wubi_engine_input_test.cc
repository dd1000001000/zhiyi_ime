// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "wubi_engine_test_support.h"
TEST(WubiEngine, engine_switch_mode) {
    std::string pinyin_path = make_temp_path("test_wubi_engine_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_engine_wubi.bin");

    cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}});
    cxxime::Dict::create_test_dict(wubi_path, {
        {"a", "工", 300},
        {"aaaa", "工", 300},
    });

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));

    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));

    // 初始应为拼音模式
    engine.switch_mode(cxxime::InputMode::PINYIN);

    // 切换到五笔模式
    engine.switch_mode(cxxime::InputMode::WUBI);

    // 切换回拼音模式
    engine.switch_mode(cxxime::InputMode::PINYIN);

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_input_flow) {
    std::string pinyin_path = make_temp_path("test_wubi_flow_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_flow_wubi.bin");

    cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}});
    cxxime::Dict::create_test_dict(wubi_path, {
        {"a", "工", 300},
        {"aaaa", "工", 300},
        {"aa", "式", 200},
    });

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));

    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));

    // 切换到五笔模式
    engine.switch_mode(cxxime::InputMode::WUBI);

    // 输入字母 a
    auto r = engine.process_key(make_key('A'));
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);

    // 应有候选（"工" 或 "式"）
    auto& ctx = engine.context();
    ASSERT_GE(ctx.candidate_page().candidates.size(), 1u);

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_paginates_from_visible_candidate_count_without_skipping) {
    std::string pinyin_path = make_temp_path("test_wubi_page_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_page_wubi.bin");
    cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}});
    const std::vector<std::tuple<std::string, std::string, int>> entries = {
        {"a", "一", 1200}, {"aa", "二", 1100}, {"ab", "三", 1000},
        {"ac", "四", 900}, {"ad", "五", 800},  {"ae", "六", 700},
        {"af", "七", 600}, {"ag", "八", 500},  {"ah", "九", 400},
    };
    cxxime::Dict::create_test_dict(wubi_path, entries);

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    cxxime::Config config;
    config.page_size = 7;
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, config, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    ASSERT_EQ(engine.process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().candidate_page().candidates.size(), 7u);
    ASSERT_TRUE(engine.context().translation().extent.complete);
    ASSERT_EQ(engine.context().translation().extent.state,
        cxxime::CandidateExtentState::kHasMore);
    std::string expected_second_page_first = engine.context().candidate_page().candidates[2].text;

    cxxime::OutputOptions options;
    ASSERT_EQ(engine.process_key(make_key(VK_NEXT), options, 2), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().page_index(), 1);
    ASSERT_EQ(engine.context().page_offset(), 2);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, expected_second_page_first);
    ASSERT_TRUE(engine.context().translation().extent.complete);

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_auto_commit) {
    std::string pinyin_path = make_temp_path("test_wubi_auto_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_auto_wubi.bin");

    cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}});
    cxxime::Dict::create_test_dict(wubi_path, {
        {"abcd", "中", 300},  // 唯一四码候选
    });

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));

    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));

    // 切换到五笔模式
    engine.switch_mode(cxxime::InputMode::WUBI);

    // 输入 abcd（四码）
    engine.process_key(make_key('A'));
    engine.process_key(make_key('B'));
    engine.process_key(make_key('C'));
    auto r = engine.process_key(make_key('D'));

    // 四码唯一候选应自动上屏
    ASSERT_EQ(r, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.context().committed_text, "中");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_page_single_candidate_does_not_auto_commit) {
    std::string pinyin_path = make_temp_path("test_wubi_page_single_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_page_single_wubi.bin");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {
                                                              {"abcd", "甲", 300},
                                                              {"abcd", "乙", 200},
                                                              {"abcd", "丙", 100},
                                                          }));

    cxxime::Config config;
    config.page_size = 2;
    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, config, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    ASSERT_EQ(engine.process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('B')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('C')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('D')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(engine.context().committed_text.empty());
    ASSERT_EQ(engine.context().page_offset(), 0);
    ASSERT_EQ(engine.context().candidate_page().candidates.size(), 2u);

    ASSERT_EQ(engine.process_key(make_key(VK_OEM_PLUS)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(engine.context().committed_text.empty());
    ASSERT_EQ(engine.context().page_offset(), 2);
    ASSERT_EQ(engine.context().candidate_page().candidates.size(), 1u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "丙");
    ASSERT_TRUE(engine.context().is_composing());

    ASSERT_EQ(engine.process_key(make_key(VK_SPACE)), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.context().committed_text, "丙");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_fifth_key_commits_first_and_starts_next_code) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_key_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_key_wubi.bin");
    std::string wubi_user_path = make_temp_path("test_wubi_fifth_key_user.tsv");
    DeleteFileA(wubi_user_path.c_str());

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {
                                                    {"abcd", "首选", 300},
                                                    {"abcd", "次选", 200},
                                                    {"e", "下一项", 100},
    }));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path, wubi_user_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    ASSERT_EQ(engine.process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('B')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('C')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('D')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 2u);

    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.context().committed_text, "首选");
    ASSERT_EQ(engine.context().active_input(), "e");
    ASSERT_TRUE(engine.context().is_composing());
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "下一项");

    const auto committed = engine.take_commit_text_with_source();
    ASSERT_EQ(committed.first, "首选");
    ASSERT_EQ(committed.second, cxxime::CommitSource::kCandidate);
    ASSERT_EQ(engine.context().active_input(), "e");
    ASSERT_TRUE(engine.context().is_composing());

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
    DeleteFileA(wubi_user_path.c_str());
}

TEST(WubiEngine, engine_wubi_fifth_key_commit_can_be_disabled) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_key_off_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_key_off_wubi.bin");
    std::string wubi_user_path = make_temp_path("test_wubi_fifth_key_off_user.tsv");
    DeleteFileA(wubi_user_path.c_str());

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {
                                                {"abcd", "首选", 300},
                                                {"abcd", "次选", 200},
    }));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path, wubi_user_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);
    cxxime::Config config;
    config.wubi_commit_first_on_fifth_key = false;
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, config, wubi_dict));

    ASSERT_EQ(engine.process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('B')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('C')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('D')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(engine.context().committed_text.empty());
    ASSERT_EQ(engine.context().active_input(), "abcde");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
    DeleteFileA(wubi_user_path.c_str());
}

TEST(WubiEngine, engine_wubi_fifth_key_restarts_after_four_code_miss) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_restart_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_restart_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"e", "新编码", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "zzzz");
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());

    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "e");
    ASSERT_EQ(engine.context().candidate_page().candidates.size(), 1u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "新编码");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, fifth_key_restart_survives_inline_ascii_round_trip) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_inline_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_inline_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"e", "新编码", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    for (int index = 0; index < 4; ++index) {
        ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(engine.process_key(make_key(VK_OEM_PLUS, true)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "zzzz+");
    ASSERT_EQ(engine.context().composition_scheme(), cxxime::CompositionScheme::kInlineAscii);

    ASSERT_EQ(engine.process_key(make_key(VK_BACK)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "zzzz");
    ASSERT_EQ(engine.context().composition_scheme(), cxxime::CompositionScheme::kWubi);
    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "e");
    ASSERT_EQ(engine.context().candidate_page().candidates.size(), 1u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "新编码");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, technical_symbols_use_inline_ascii_in_wubi_and_mixed_modes) {
    std::string pinyin_path = make_temp_path("test_inline_modes_pinyin.bin");
    std::string wubi_path = make_temp_path("test_inline_modes_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"c", "拼音", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"c", "五笔", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));

    for (cxxime::InputMode mode : {cxxime::InputMode::WUBI}) {
        engine.clear();
        engine.switch_mode(mode);
        ASSERT_EQ(engine.process_key(make_key('C')), cxxime::ProcessResult::ACCEPTED);
        ASSERT_TRUE(!engine.context().candidate_page().candidates.empty());
        ASSERT_EQ(engine.process_key(make_key(VK_OEM_PLUS, true)), cxxime::ProcessResult::ACCEPTED);
        ASSERT_EQ(engine.process_key(make_key(VK_ADD)), cxxime::ProcessResult::ACCEPTED);
        ASSERT_EQ(engine.context().active_input(), "c++");
        ASSERT_EQ(engine.context().composition_scheme(),
                  cxxime::CompositionScheme::kInlineAscii);
        ASSERT_TRUE(engine.context().candidate_page().candidates.empty());
    }

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, inline_ascii_space_cancels_and_enter_commits) {
    std::string pinyin_path = make_temp_path("test_inline_space_pinyin.bin");
    std::string wubi_path = make_temp_path("test_inline_space_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "pinyin", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"c", "wubi", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    for (uint32_t key : {'H', 'S', 'Q'}) {
        ASSERT_EQ(engine.process_key(make_key(key)), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());
    ASSERT_EQ(engine.process_key(make_key(VK_DIVIDE)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key(VK_MULTIPLY)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "hsq/*");

    ASSERT_EQ(engine.process_key(make_key(VK_SPACE)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(!engine.context().is_composing());
    ASSERT_TRUE(engine.context().committed_text.empty());

    for (uint32_t key : {'H', 'S', 'Q'}) {
        ASSERT_EQ(engine.process_key(make_key(key)), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(engine.process_key(make_key(VK_DIVIDE)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key(VK_MULTIPLY)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key(VK_RETURN)), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "hsq/*");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, fifth_key_restart_requires_cursor_at_end) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_cursor_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_cursor_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"e", "新编码", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    for (int index = 0; index < 4; ++index) {
        ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(engine.process_key(make_key(VK_LEFT)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "zzzez");

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_fifth_key_empty_restart_can_be_disabled) {
    std::string pinyin_path = make_temp_path("test_wubi_fifth_restart_off_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_fifth_restart_off_wubi.bin");

    ASSERT_TRUE(cxxime::Dict::create_test_dict(pinyin_path, {{"a", "拼", 100}}));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(wubi_path, {{"e", "新编码", 300}}));

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));
    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);
    cxxime::Config config;
    config.wubi_restart_on_fifth_after_miss = false;
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, config, wubi_dict));

    for (int index = 0; index < 4; ++index) {
        ASSERT_EQ(engine.process_key(make_key('Z')), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(engine.process_key(make_key('E')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "zzzze");
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_wubi_space_without_candidates_clears) {
    std::string pinyin_path = make_temp_path("test_wubi_space_clear_pinyin.bin");
    std::string wubi_path = make_temp_path("test_wubi_space_clear_wubi.bin");

    cxxime::Dict::create_test_dict(pinyin_path, {{"ni:hao", "你好", 100}});
    cxxime::Dict::create_test_dict(wubi_path, {{"a", "工", 300}});

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));

    auto wubi_dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    ASSERT_TRUE(wubi_dict->open(wubi_path));
    ASSERT_TRUE(test::apply_runtime(engine, pinyin_path, cxxime::Config{}, wubi_dict));
    engine.switch_mode(cxxime::InputMode::WUBI);

    engine.process_key(make_key('N'));
    engine.process_key(make_key('I'));
    engine.process_key(make_key('W'));
    auto r = engine.process_key(make_key('E'));
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(engine.context().is_composing());
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());

    r = engine.process_key(make_key(VK_SPACE));
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(!engine.context().is_composing());
    ASSERT_TRUE(engine.context().committed_text.empty());
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());

    engine.finalize();
    wubi_dict->close();
    DeleteFileA(pinyin_path.c_str());
    DeleteFileA(wubi_path.c_str());
}

TEST(WubiEngine, engine_no_wubi_dict_fallback) {
    std::string pinyin_path = make_temp_path("test_wubi_fallback.bin");
    cxxime::Dict::create_test_dict(pinyin_path, {{"a", "啊", 100}});

    cxxime::Engine engine;
    ASSERT_TRUE(engine.initialize(pinyin_path));

    // 不设置五笔词典，尝试切换到五笔模式
    engine.switch_mode(cxxime::InputMode::WUBI);

    // 应自动回退到拼音模式
    auto r = engine.process_key(make_key('A'));
    ASSERT_EQ(r, cxxime::ProcessResult::ACCEPTED);

    // 应有拼音候选
    auto& ctx = engine.context();
    ASSERT_GE(ctx.candidate_page().candidates.size(), 1u);

    engine.finalize();
    DeleteFileA(pinyin_path.c_str());
}
