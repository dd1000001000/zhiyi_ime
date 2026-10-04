// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "engine_test_support.h"
TEST(Engine, shift_toggle_then_capslock_uppercase) {
    // End-to-end: Shift toggles to English, CapsLock ON, type letter → uppercase
    std::string dict_path = make_temp_path("test_shift_caps_dict.bin");
    std::string spellings_path = make_temp_path("test_shift_caps_spellings.bin");
    cxxime::Dict::create_test_dict(dict_path, {{"a", "啊", 100}});
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {{"a", "a", 0, 0.0f}}));

    cxxime::Config config;
    config.ascii_switch_key["Shift_L"] = "code";
    config.ascii_switch_key["Caps_Lock"] = "clear";

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, config, spellings_path));

    // Step 1: Press and release Shift_L → toggles to English mode
    cxxime::KeyEvent shift_down;
    shift_down.keycode = 0xA0;  // VK_LSHIFT
    shift_down.is_key_up = false;
    engine.process_key(shift_down);

    cxxime::KeyEvent shift_up;
    shift_up.keycode = 0xA0;
    shift_up.is_key_up = true;
    engine.process_key(shift_up);

    ASSERT_TRUE(engine.ascii_composer().is_ascii_mode());

    // Step 2: Press CapsLock (OS toggles ON) — should NOT flip mode back
    cxxime::KeyEvent caps_down;
    caps_down.keycode = 0x14;  // VK_CAPITAL
    caps_down.is_key_up = false;
    caps_down.set_caps_lock();  // CapsLock now ON
    engine.process_key(caps_down);

    ASSERT_TRUE(engine.ascii_composer().is_ascii_mode());  // still English

    // Step 3: Type letter with CapsLock ON → should be uppercase
    cxxime::KeyEvent letter;
    letter.keycode = 'N';
    letter.is_key_up = false;
    letter.modifiers = 0x08;  // CapsLock ON

    auto result = engine.process_key(letter);
    ASSERT_EQ(result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "N");

    cxxime::KeyEvent caps_up;
    caps_up.keycode = 0x14;
    caps_up.is_key_up = false;
    engine.process_key(caps_up);

    ASSERT_TRUE(engine.ascii_composer().is_ascii_mode());

    cxxime::KeyEvent lower_letter;
    lower_letter.keycode = 'I';
    lower_letter.is_key_up = false;

    result = engine.process_key(lower_letter);
    // Laya: with the English word list installed, English mode opens a word for completion;
    // Enter commits it as typed.
    if (result == cxxime::ProcessResult::ACCEPTED) {
        cxxime::KeyEvent enter;
        enter.keycode = VK_RETURN;
        result = engine.process_key(enter);
    }
    ASSERT_EQ(result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "i");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, ascii_mode_shift_capslock_lowercase) {
    // Shift+CapsLock in ASCII mode → lowercase (they cancel)
    std::string dict_path = make_temp_path("test_ascii_sc_dict.bin");
    std::string spellings_path = make_temp_path("test_ascii_sc_spellings.bin");
    cxxime::Dict::create_test_dict(dict_path, {{"a", "啊", 100}});
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {{"a", "a", 0, 0.0f}}));

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, cxxime::Config{}, spellings_path));
    engine.ascii_composer().set_ascii_mode(true);

    cxxime::KeyEvent event;
    event.keycode = 'N';
    event.is_key_up = false;
    event.modifiers = 0x09;  // CapsLock (0x08) + Shift (0x01)

    auto result = engine.process_key(event);
    ASSERT_EQ(result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "n");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, ascii_mode_enter_passes_to_application) {
    std::string dict_path = make_temp_path("test_ascii_enter_dict.bin");
    std::string spellings_path = make_temp_path("test_ascii_enter_spellings.bin");
    cxxime::Dict::create_test_dict(dict_path, {{"a", "a", 100}});
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {{"a", "a", 0, 0.0f}}));

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, cxxime::Config{}, spellings_path));
    engine.ascii_composer().set_ascii_mode(true);

    cxxime::KeyEvent enter;
    enter.keycode = VK_RETURN;
    enter.is_key_up = false;

    auto result = engine.process_key(enter);
    ASSERT_EQ(result, cxxime::ProcessResult::REJECTED);
    ASSERT_TRUE(engine.get_commit_text().empty());

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, capslock_overlay_shift_keeps_ascii_mode) {
    std::string dict_path = make_temp_path("test_caps_shift_overlay_dict.bin");
    std::string spellings_path = make_temp_path("test_caps_shift_overlay_spellings.bin");
    cxxime::Dict::create_test_dict(dict_path, {{"ni", "ni", 100}});
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {{"ni", "ni", 0, 0.0f}}));

    cxxime::Config config;
    config.ascii_switch_key["Shift_L"] = "code";
    config.ascii_switch_key["Caps_Lock"] = "clear";

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, config, spellings_path));

    cxxime::KeyEvent caps_down;
    caps_down.keycode = 0x14;  // VK_CAPITAL
    caps_down.is_key_up = false;
    caps_down.set_caps_lock();
    engine.process_key(caps_down);
    ASSERT_TRUE(engine.ascii_composer().is_ascii_mode());

    cxxime::KeyEvent shift_down;
    shift_down.keycode = VK_LSHIFT;
    shift_down.is_key_up = false;
    shift_down.modifiers = 0x09;  // Shift + CapsLock
    engine.process_key(shift_down);

    cxxime::KeyEvent shift_up = shift_down;
    shift_up.is_key_up = true;
    engine.process_key(shift_up);
    ASSERT_TRUE(engine.ascii_composer().is_ascii_mode());

    cxxime::KeyEvent n;
    n.keycode = 'N';
    n.is_key_up = false;
    n.modifiers = 0x08;
    auto result = engine.process_key(n);
    ASSERT_EQ(result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "N");

    cxxime::KeyEvent i;
    i.keycode = 'I';
    i.is_key_up = false;
    i.modifiers = 0x08;
    result = engine.process_key(i);
    ASSERT_EQ(result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "I");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

// --- CapsLock v2: append / candidate / code / clear mode tests ---

TEST(AsciiComposer, capslock_append_noop_during_composing) {
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "append";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    ASSERT_TRUE(ctx.set_preedit("nihao"));
    ASSERT_TRUE(ctx.is_composing());

    // Press CapsLock in append mode — should NOT clear or toggle
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON

    ASSERT_TRUE(!ac.is_ascii_mode());  // still Chinese mode
    ASSERT_EQ(ctx.active_input(), "nihao");  // buffer unchanged
}

TEST(AsciiComposer, capslock_candidate_commits_first_candidate) {
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "candidate";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    // Set up composing state with candidates
    ASSERT_TRUE(ctx.set_preedit("nihao"));
    cxxime::CandidatePage page;
    cxxime::Candidate c1; c1.text = "你好";
    cxxime::Candidate c2; c2.text = "拟好";
    page.candidates.push_back(c1);
    page.candidates.push_back(c2);
    page.highlighted = 0;
    ctx.update_candidates(std::move(page));

    // Press CapsLock — should commit first candidate and toggle
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON

    ASSERT_TRUE(ac.is_ascii_mode());
    ASSERT_EQ(ctx.committed_text, "你好");
    ASSERT_TRUE(ctx.active_input().empty());
}

TEST(AsciiComposer, capslock_candidate_no_candidates_toggles) {
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "candidate";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    ASSERT_TRUE(ctx.set_preedit("zzz"));  // no valid candidates
    ASSERT_TRUE(ctx.is_composing());

    // Press CapsLock - no candidates, commit raw text and toggle
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON

    ASSERT_TRUE(ac.is_ascii_mode());
    ASSERT_TRUE(ctx.active_input().empty());
    ASSERT_EQ(ctx.committed_text, "zzz");
    ASSERT_EQ(ctx.commit_source(), cxxime::CommitSource::kRawCodePreserveCase);
}

TEST(AsciiComposer, capslock_code_commits_buffer) {
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "code";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    ASSERT_TRUE(ctx.set_preedit("nihao"));

    // Press CapsLock — should commit raw buffer and toggle
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON

    ASSERT_TRUE(ac.is_ascii_mode());
    ASSERT_EQ(ctx.committed_text, "nihao");
    ASSERT_TRUE(ctx.active_input().empty());
}

TEST(Engine, capslock_append_letter_accepted) {
    // Append mode while composing: CapsLock + letter should be ACCEPTED
    // (goes to buffer), not COMMITTED.
    // Need a real dictionary so translator doesn't crash on buffer query
    std::string dict_path = make_temp_path("test_append_dict.bin");
    std::string spellings_path = make_temp_path("test_append_spellings.bin");

    cxxime::Dict::create_test_dict(dict_path, {
        {"a", "啊", 100},
        {"n", "嗯", 100},
    });
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {
        {"a", "a", 0, 0.0f},
        {"n", "n", 0, 0.0f},
    }));

    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "append";

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, config, spellings_path));

    cxxime::KeyEvent first;
    first.keycode = 'N';
    first.is_key_up = false;
    ASSERT_EQ(engine.process_key(first), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "n");

    // Press CapsLock while composing. Append mode should not clear or switch.
    cxxime::KeyEvent caps_event;
    caps_event.keycode = 0x14;  // VK_CAPITAL
    caps_event.is_key_up = false;
    caps_event.set_caps_lock();  // OS has toggled CapsLock ON
    engine.process_key(caps_event);
    ASSERT_TRUE(!engine.ascii_composer().is_ascii_mode());
    ASSERT_EQ(engine.context().active_input(), "n");

    // Now press 'A' with CapsLock ON
    cxxime::KeyEvent event;
    event.keycode = 'A';
    event.is_key_up = false;
    event.modifiers = 0x08;  // CapsLock ON

    auto result = engine.process_key(event);
    // In append mode, letter should be accepted (buffered), not committed
    ASSERT_EQ(result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(engine.context().active_input(), "nA");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, capslock_append_clears_candidates_and_commits_raw_code) {
    std::string dict_path = make_temp_path("test_append_raw_dict.bin");
    std::string spellings_path = make_temp_path("test_append_raw_spellings.bin");

    cxxime::Dict::create_test_dict(dict_path, {
        {"ni", "你", 100},
    });
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {
        {"ni", "ni", 0, 0.0f},
    }));

    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "append";

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, config, spellings_path));

    for (char ch : std::string("NI")) {
        cxxime::KeyEvent event;
        event.keycode = ch;
        event.is_key_up = false;
        ASSERT_EQ(engine.process_key(event), cxxime::ProcessResult::ACCEPTED);
    }
    ASSERT_EQ(engine.context().active_input(), "ni");
    ASSERT_TRUE(!engine.context().candidate_page().candidates.empty());

    cxxime::KeyEvent caps_event;
    caps_event.keycode = 0x14;  // VK_CAPITAL
    caps_event.is_key_up = false;
    caps_event.set_caps_lock();
    engine.process_key(caps_event);

    for (char ch : std::string("DD")) {
        cxxime::KeyEvent event;
        event.keycode = ch;
        event.is_key_up = false;
        event.set_caps_lock();
        ASSERT_EQ(engine.process_key(event), cxxime::ProcessResult::ACCEPTED);
    }

    ASSERT_EQ(engine.context().active_input(), "niDD");
    ASSERT_TRUE(engine.context().candidate_page().candidates.empty());

    cxxime::KeyEvent space;
    space.keycode = VK_SPACE;
    space.is_key_up = false;
    space.set_caps_lock();
    ASSERT_EQ(engine.process_key(space), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "niDD");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, capslock_append_enter_preserves_case) {
    std::string dict_path = make_temp_path("test_append_enter_dict.bin");
    std::string spellings_path = make_temp_path("test_append_enter_spellings.bin");

    cxxime::Dict::create_test_dict(dict_path, {
        {"ni", "你", 100},
    });
    ASSERT_TRUE(cxxime::SpellingsIndex::create_test_trie(spellings_path, {
        {"ni", "ni", 0, 0.0f},
    }));

    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "append";

    cxxime::Engine engine;
    ASSERT_TRUE(test::initialize_engine(engine, dict_path, config, spellings_path));

    for (char ch : std::string("NI")) {
        cxxime::KeyEvent event;
        event.keycode = ch;
        event.is_key_up = false;
        ASSERT_EQ(engine.process_key(event), cxxime::ProcessResult::ACCEPTED);
    }

    cxxime::KeyEvent caps_event;
    caps_event.keycode = 0x14;
    caps_event.is_key_up = false;
    caps_event.set_caps_lock();
    engine.process_key(caps_event);

    for (char ch : std::string("DD")) {
        cxxime::KeyEvent event;
        event.keycode = ch;
        event.is_key_up = false;
        event.set_caps_lock();
        ASSERT_EQ(engine.process_key(event), cxxime::ProcessResult::ACCEPTED);
    }

    cxxime::KeyEvent enter;
    enter.keycode = VK_RETURN;
    enter.is_key_up = false;
    enter.set_caps_lock();
    ASSERT_EQ(engine.process_key(enter), cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(engine.get_commit_text(), "niDD");

    engine.finalize();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(spellings_path.c_str());
}

TEST(Engine, capslock_candidate_not_downgraded) {
    // "candidate" should NOT be downgraded to "clear" (unlike inline_ascii)
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "candidate";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    // Verify binding is still CANDIDATE, not CLEAR
    ASSERT_TRUE(ac.get_binding(0x14) == cxxime::AsciiModeSwitchStyle::CANDIDATE);
}

TEST(Engine, capslock_append_not_downgraded) {
    // "append" should NOT be downgraded
    cxxime::Config config;
    config.ascii_switch_key["Caps_Lock"] = "append";

    cxxime::AsciiComposer ac;
    ac.load_config(config);

    ASSERT_TRUE(ac.get_binding(0x14) == cxxime::AsciiModeSwitchStyle::APPEND);
}

// --- CapsLock + Shift interaction tests ---

TEST(AsciiComposer, capslock_on_shift_does_not_toggle) {
    // CapsLock ON 时按 Shift 松开，不应切换模式（Shift 用于大小写反转）
    cxxime::Config config;
    config.ascii_switch_key["Shift_L"] = "code";
    config.ascii_switch_key["Caps_Lock"] = "clear";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    // Step 1: CapsLock ON → toggles to English
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON
    ASSERT_TRUE(ac.is_ascii_mode());

    // Step 2: Shift down (CapsLock still ON)
    ac.process_key(VK_LSHIFT, false, ctx, true);  // caps_lock = true

    // Step 3: Shift up must not toggle Chinese/English while CapsLock is ON.
    ac.process_key(VK_LSHIFT, true, ctx, true);  // caps_lock = true
    ASSERT_TRUE(ac.is_ascii_mode());
}

TEST(AsciiComposer, shift_held_capslock_no_double_toggle) {
    // Shift 按住→CapsLock ON→松 Shift，只切换一次（CapsLock 那次）
    cxxime::Config config;
    config.ascii_switch_key["Shift_L"] = "code";
    config.ascii_switch_key["Caps_Lock"] = "clear";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    // Step 1: Shift down
    ac.process_key(VK_LSHIFT, false, ctx, false);  // caps_lock = false
    ASSERT_TRUE(!ac.is_ascii_mode());  // still Chinese

    // Step 2: CapsLock ON (while Shift held) → toggles to English
    ac.process_key(0x14, false, ctx, true);  // VK_CAPITAL down, CapsLock ON
    ASSERT_TRUE(ac.is_ascii_mode());

    // Step 3: Shift up → should NOT toggle again (caps_lock = true)
    ac.process_key(VK_LSHIFT, true, ctx, true);
    ASSERT_TRUE(ac.is_ascii_mode());  // still English, no double-toggle
}

TEST(AsciiComposer, shift_toggle_still_works_without_capslock) {
    // CapsLock OFF 时 Shift 仍然正常切换模式
    cxxime::Config config;
    config.ascii_switch_key["Shift_L"] = "code";

    cxxime::AsciiComposer ac;
    ac.load_config(config);
    cxxime::Context ctx;

    // Shift down → Shift up → toggles to English
    ac.process_key(VK_LSHIFT, false, ctx, false);
    ac.process_key(VK_LSHIFT, true, ctx, false);
    ASSERT_TRUE(ac.is_ascii_mode());

    // Shift down → Shift up → toggles back to Chinese
    ac.process_key(VK_LSHIFT, false, ctx, false);
    ac.process_key(VK_LSHIFT, true, ctx, false);
    ASSERT_TRUE(!ac.is_ascii_mode());
}

// Initialize temp_path before tests run
static bool _engine_init = []() {
    GetTempPathA(MAX_PATH, engine_test_temp_path);
    return true;
}();
