// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "engine_test_support.h"
TEST(Engine, process_letter_key) {
    cxxime::Engine engine;
    cxxime::Context ctx;

    cxxime::KeyEvent event;
    event.keycode = 'N';
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(ctx.active_input(), "n");
}

TEST(Engine, input_code_stops_at_shared_limit) {
    cxxime::Context context;
    cxxime::PinyinProcessor processor;
    cxxime::KeyEvent letter;
    letter.keycode = 'A';

    for (size_t i = 0; i < cxxime::kMaxInputCodeLength; ++i) {
        ASSERT_EQ(processor.process_key(letter, context), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(context.active_input().size(), cxxime::kMaxInputCodeLength);
    const uint64_t revision = context.preedit_revision();

    ASSERT_EQ(processor.process_key(letter, context), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(context.active_input().size(), cxxime::kMaxInputCodeLength);
    ASSERT_EQ(context.preedit_revision(), revision);

    cxxime::KeyEvent backspace;
    backspace.keycode = VK_BACK;
    ASSERT_EQ(processor.process_key(backspace, context), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(context.active_input().size(), cxxime::kMaxInputCodeLength - 1);
    ASSERT_EQ(processor.process_key(letter, context), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(context.active_input().size(), cxxime::kMaxInputCodeLength);
}

TEST(Engine, normalizes_printable_ascii_keys) {
    struct Case {
        uint32_t keycode;
        bool shift;
        char expected;
    };
    const Case cases[] = {
        {'A', false, 'a'},
        {'A', true, 'A'},
        {'0', false, '0'},
        {'0', true, ')'},
        {'1', true, '!'},
        {'2', true, '@'},
        {'3', true, '#'},
        {'4', true, '$'},
        {'5', true, '%'},
        {'6', true, '^'},
        {'7', true, '&'},
        {'8', true, '*'},
        {'9', true, '('},
        {VK_SPACE, false, ' '},
        {VK_OEM_PERIOD, false, '.'},
        {VK_OEM_PERIOD, true, '>'},
        {VK_OEM_COMMA, false, ','},
        {VK_OEM_COMMA, true, '<'},
        {VK_OEM_1, false, ';'},
        {VK_OEM_1, true, ':'},
        {VK_OEM_2, false, '/'},
        {VK_OEM_2, true, '?'},
        {VK_OEM_3, false, '`'},
        {VK_OEM_3, true, '~'},
        {VK_OEM_4, false, '['},
        {VK_OEM_4, true, '{'},
        {VK_OEM_5, false, '\\'},
        {VK_OEM_5, true, '|'},
        {VK_OEM_6, false, ']'},
        {VK_OEM_6, true, '}'},
        {VK_OEM_7, false, '\''},
        {VK_OEM_7, true, '"'},
        {VK_OEM_MINUS, false, '-'},
        {VK_OEM_MINUS, true, '_'},
        {VK_OEM_PLUS, false, '='},
        {VK_OEM_PLUS, true, '+'},
        {VK_NUMPAD0, false, '0'},
        {VK_NUMPAD9, false, '9'},
        {VK_ADD, false, '+'},
        {VK_SUBTRACT, false, '-'},
        {VK_MULTIPLY, false, '*'},
        {VK_DIVIDE, false, '/'},
        {VK_DECIMAL, false, '.'},
    };
    for (const Case& item : cases) {
        cxxime::KeyEvent event;
        event.keycode = item.keycode;
        if (item.shift) {
            event.set_shift();
        }
        ASSERT_TRUE(cxxime::normalize_ascii_key(event).has_value());
        ASSERT_EQ(*cxxime::normalize_ascii_key(event), item.expected);
    }

    cxxime::KeyEvent caps;
    caps.keycode = 'A';
    caps.set_caps_lock();
    ASSERT_EQ(*cxxime::normalize_ascii_key(caps), 'A');
    caps.set_shift();
    ASSERT_EQ(*cxxime::normalize_ascii_key(caps), 'a');

    cxxime::KeyEvent shortcut;
    shortcut.keycode = 'A';
    shortcut.set_ctrl();
    ASSERT_TRUE(!cxxime::normalize_ascii_key(shortcut).has_value());
    shortcut.modifiers = 0;
    shortcut.set_alt();
    ASSERT_TRUE(!cxxime::normalize_ascii_key(shortcut).has_value());
    shortcut.modifiers = 0;
    shortcut.is_key_up = true;
    ASSERT_TRUE(!cxxime::normalize_ascii_key(shortcut).has_value());
}

TEST(Engine, translator_excludes_candidate_text_over_shared_capacity) {
    std::string dict_path = make_temp_path("test_candidate_text_capacity.bin");
    std::string accepted;
    std::string rejected;
    for (size_t i = 0; i < 64; ++i) {
        accepted += "界";
    }
    for (size_t i = 0; i < 86; ++i) {
        rejected += "界";
    }
    ASSERT_TRUE(cxxime::candidate_text_fits(accepted));
    ASSERT_TRUE(!cxxime::candidate_text_fits(rejected));
    ASSERT_TRUE(cxxime::Dict::create_test_dict(
        dict_path, {{"abc", rejected, 2000}, {"abc", accepted, 1000}}));

    cxxime::Dict dict{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dict.open_dict(dict_path));
    cxxime::WubiTranslator translator;
    translator.set_dict(&dict);
    auto page = translator.translate_page("abc", 0, 10);
    ASSERT_EQ(page.candidates.size(), static_cast<size_t>(1));
    ASSERT_TRUE(page.candidates[0].text == accepted);
    ASSERT_TRUE(page.candidates[0].text.size() > 63);

    dict.close();
    DeleteFileA(dict_path.c_str());
}

TEST(Engine, process_escape) {
    cxxime::Context ctx;
    ASSERT_TRUE(ctx.set_preedit("nihao"));

    cxxime::KeyEvent event;
    event.keycode = VK_ESCAPE;
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(ctx.active_input().empty());
}

TEST(Engine, process_backspace) {
    cxxime::Context ctx;
    ASSERT_TRUE(ctx.set_preedit("ni"));

    cxxime::KeyEvent event;
    event.keycode = VK_BACK;
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(ctx.active_input(), "n");
}

TEST(Engine, translate_dd_has_candidates) {
    std::string dict_path = make_temp_path("test_engine_dict.bin");
    std::string spellings_path = make_temp_path("test_engine_spellings.bin");

    cxxime::Dict::create_test_dict(dict_path, {
        {"di:di", "弟弟", 500},
        {"da:da", "大大", 400},
        {"de:dao", "得到", 300},
    });

    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {
        {"d", "da", 2, -0.693f},
        {"d", "di", 2, -0.693f},
        {"d", "de", 2, -0.693f},
        {"da", "da", 0, 0.0f},
        {"di", "di", 0, 0.0f},
        {"de", "de", 0, 0.0f},
    }));

    cxxime::Dict dict{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dict.open_dict(dict_path));

    auto pinyin_resources = cxxime::PinyinResourceSet::create(
        "full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin, spellings_path);
    ASSERT_TRUE(pinyin_resources != nullptr);

    cxxime::PinyinTranslator translator;
    translator.set_dict(&dict);
    translator.bind_pinyin(pinyin_resources, {});

    auto page = translator.translate_page("dd", 0, 10);
    ASSERT_GE(page.candidates.size(), 1u);

    bool found_didi = false;
    for (const auto& c : page.candidates) {
        if (c.text == "弟弟") found_didi = true;
    }
    ASSERT_TRUE(found_didi);
    ASSERT_EQ(page.highlighted, 0);

    dict.close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, selected_pinyin_candidate_records_typed_code_as_preference) {
    std::string dict_path = make_temp_path("test_engine_learn_syllables.bin");
    std::string user_path = make_temp_path("test_engine_learn_syllables.tsv");
    DeleteFileA(user_path.c_str());

    cxxime::Dict::create_test_dict(dict_path, {
        {"shu:ru:fa", "测试系统词", 300},
    });

    auto dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(dict->open(dict_path, user_path));

    cxxime::Config config;
    config.candidate_learning = true;
    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict, config));

    for (char ch : std::string("SHURUFA")) {
        cxxime::KeyEvent event;
        event.keycode = ch;
        event.is_key_up = false;
        ASSERT_EQ(engine.process_key(event), cxxime::ProcessResult::ACCEPTED);
    }

    const auto candidates = engine.context().candidate_page().candidates;
    ASSERT_GE(candidates.size(), 1u);
    ASSERT_EQ(candidates[0].text, "测试系统词");
    ASSERT_TRUE(engine.select_candidate(0));

    auto learned = dict->query_candidate_preferences("测试系统词", 0, 10);
    ASSERT_EQ(learned.size(), static_cast<size_t>(1));
    ASSERT_EQ(learned[0].code, "shurufa");
    ASSERT_EQ(dict->user_entry_count(), static_cast<size_t>(0));

    engine.finalize();
    dict->close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(Engine, candidate_order_stays_stable_when_candidate_learning_is_disabled) {
    std::string dict_path = make_temp_path("test_engine_stable_order.bin");
    std::string user_path = make_temp_path("test_engine_stable_order.tsv");
    DeleteFileA(user_path.c_str());

    cxxime::Dict::create_test_dict(dict_path, {
        {"ni:hao", "默认候选", 300},
        {"ni:hao", "第二候选", 200},
    });

    auto dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(dict->open(dict_path, user_path));
    cxxime::Config config;
    config.candidate_learning = false;  // self-learning is on by default
    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict, config));

    type_code(engine, "nihao");
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 2u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "默认候选");
    ASSERT_EQ(engine.context().candidate_page().candidates[1].text, "第二候选");
    ASSERT_TRUE(engine.select_candidate(1));
    ASSERT_EQ(engine.get_commit_text(), "第二候选");
    ASSERT_TRUE(!dict->has_user_entry("第二候选"));
    ASSERT_EQ(dict->candidate_preference_count(), static_cast<size_t>(0));

    type_code(engine, "nihao");
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 2u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "默认候选");
    ASSERT_EQ(engine.context().candidate_page().candidates[1].text, "第二候选");
    cxxime::KeyEvent space;
    space.keycode = VK_SPACE;
    space.is_key_up = false;
    ASSERT_EQ(engine.process_key(space), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "默认候选");
    ASSERT_TRUE(!dict->has_user_entry("默认候选"));
    ASSERT_EQ(dict->candidate_preference_count(), static_cast<size_t>(0));

    engine.finalize();
    dict->close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(Engine, candidate_learning_promotes_selected_candidate_when_enabled) {
    std::string dict_path = make_temp_path("test_engine_adaptive_order.bin");
    std::string user_path = make_temp_path("test_engine_adaptive_order.tsv");
    DeleteFileA(user_path.c_str());

    cxxime::Dict::create_test_dict(dict_path, {
        {"ni:hao", "默认候选", 300},
        {"ni:hao", "第二候选", 200},
    });

    auto dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(dict->open(dict_path, user_path));
    cxxime::Config config;
    config.candidate_learning = true;
    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict, config));

    type_code(engine, "nihao");
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 2u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "默认候选");
    ASSERT_EQ(engine.context().candidate_page().candidates[1].text, "第二候选");
    ASSERT_TRUE(engine.select_candidate(1));
    ASSERT_EQ(engine.get_commit_text(), "第二候选");
    ASSERT_EQ(dict->user_entry_count(), static_cast<size_t>(0));
    ASSERT_EQ(dict->candidate_preference_count(), static_cast<size_t>(1));

    type_code(engine, "nihao");
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 1u);
    ASSERT_EQ(engine.context().candidate_page().candidates[0].text, "第二候选");

    engine.finalize();
    dict->close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(Engine, candidate_learning_uses_candidate_text_for_punctuation_commit) {
    std::string dict_path = make_temp_path("test_engine_punctuation_learning.bin");
    std::string user_path = make_temp_path("test_engine_punctuation_learning.tsv");
    DeleteFileA(user_path.c_str());

    cxxime::Dict::create_test_dict(dict_path, {
        {"ni:hao", "你好", 300},
    });

    auto dict = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    ASSERT_TRUE(dict->open(dict_path, user_path));
    cxxime::Config config;
    config.candidate_learning = true;
    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict, config));

    type_code(engine, "nihao");
    ASSERT_GE(engine.context().candidate_page().candidates.size(), 1u);

    cxxime::PunctMapping punct_mapping;
    punct_mapping.half_shape["."] = {
        cxxime::PunctType::COMMIT, "。", {}, {}};
    cxxime::OutputOptions options;
    options.punct_mapping = &punct_mapping;
    cxxime::KeyEvent period;
    period.keycode = VK_OEM_PERIOD;
    ASSERT_EQ(engine.process_key(period, options), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "你好。");
    ASSERT_TRUE(!dict->has_user_entry("你好"));
    auto preferences = dict->query_candidate_preferences("你好", 0, 10);
    ASSERT_EQ(preferences.size(), static_cast<size_t>(1));
    ASSERT_EQ(preferences[0].text, "你好");

    engine.finalize();
    dict->close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(Engine, translate_valid_pinyin) {
    std::string dict_path = make_temp_path("test_engine_valid.bin");
    cxxime::Dict::create_test_dict(dict_path, {
        {"de", "的", 1000},
        {"de:dao", "得到", 300},
    });

    cxxime::Dict dict{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dict.open_dict(dict_path));

    cxxime::PinyinTranslator translator;
    translator.set_dict(&dict);

    auto page = translator.translate_page("de", 0, 10);
    ASSERT_GE(page.candidates.size(), 1u);

    bool found_de = false;
    for (const auto& c : page.candidates) {
        if (c.text == "的") found_de = true;
    }
    ASSERT_TRUE(found_de);

    dict.close();
    DeleteFileA(dict_path.c_str());
}

TEST(Engine, space_with_candidates_commits) {
    cxxime::Context ctx;
    ASSERT_TRUE(ctx.set_preedit("de"));
    cxxime::CandidatePage page;
    page.candidates.push_back({"的", "", 100});
    page.highlighted = 0;
    ctx.update_candidates(std::move(page));

    cxxime::KeyEvent event;
    event.keycode = VK_SPACE;
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::CANDIDATE_SELECTED);
    ASSERT_EQ(ctx.take_requested_candidate_selection().value_or(-1), 0);
}

TEST(Engine, space_no_candidates_dismisses) {
    cxxime::Context ctx;
    ASSERT_TRUE(ctx.set_preedit("zzz"));

    cxxime::KeyEvent event;
    event.keycode = VK_SPACE;
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(ctx.active_input().empty());
}

TEST(Engine, number_selects_candidate) {
    cxxime::Context ctx;
    ASSERT_TRUE(ctx.set_preedit("de"));
    cxxime::CandidatePage page;
    page.candidates.push_back({"的", "", 100});
    page.candidates.push_back({"地", "", 80});
    page.highlighted = 0;
    ctx.update_candidates(std::move(page));

    cxxime::KeyEvent event;
    event.keycode = '2';
    event.is_key_up = false;

    cxxime::PinyinProcessor processor;
    auto result = processor.process_key(event, ctx);
    ASSERT_EQ(result, cxxime::ProcessResult::CANDIDATE_SELECTED);
    ASSERT_EQ(ctx.take_requested_candidate_selection().value_or(-1), 1);
}

// Verify that raw VK constants used in engine (to avoid <windows.h> dependency)
// match the actual Windows definitions.
TEST(Engine, vk_constants_match_windows) {
    ASSERT_EQ(0xA0, (uint32_t)VK_LSHIFT);
    ASSERT_EQ(0xA1, (uint32_t)VK_RSHIFT);
    ASSERT_EQ(0xA2, (uint32_t)VK_LCONTROL);
    ASSERT_EQ(0xA3, (uint32_t)VK_RCONTROL);
    ASSERT_EQ(0x14, (uint32_t)VK_CAPITAL);
    ASSERT_EQ(0x20, (uint32_t)VK_SPACE);
    ASSERT_EQ(0x0D, (uint32_t)VK_RETURN);
}

int main() {
    GetTempPathA(MAX_PATH, engine_test_temp_path);
    return test::RunAllTests();
}
