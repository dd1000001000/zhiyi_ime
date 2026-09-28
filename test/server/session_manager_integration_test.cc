// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <map>

#include "config_store.h"
#include "config_write_coordinator.h"
#include "session_manager_integration_test_support.h"

TEST(SessionIntegration, shape_and_punctuation_toggles_preserve_composition) {
    SessionManager manager;
    manager.initialize(setup_test_dict());
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    const ProcessKeyResult initial = manager.process_key(id, make_key('I'));
    ASSERT_TRUE(initial.composing);
    ASSERT_EQ(initial.preedit, "ni");
    ASSERT_TRUE(!initial.presentation.items.empty());

    const ProcessKeyResult shape = manager.process_key(id, make_key(VK_SPACE, true));
    ASSERT_EQ(shape.result, cxxime::ProcessResult::TOGGLE_SHAPE);
    ASSERT_TRUE(shape.composing);
    ASSERT_EQ(shape.preedit, initial.preedit);
    ASSERT_EQ(shape.preedit_cursor, initial.preedit_cursor);
    ASSERT_EQ(shape.presentation.items.size(), initial.presentation.items.size());
    ASSERT_TRUE(shape.ime_status.full_shape());

    cxxime::KeyEvent punctuation = make_key(VK_OEM_PERIOD);
    punctuation.set_ctrl();
    const ProcessKeyResult punct = manager.process_key(id, punctuation);
    ASSERT_EQ(punct.result, cxxime::ProcessResult::TOGGLE_PUNCT);
    ASSERT_TRUE(punct.composing);
    ASSERT_EQ(punct.preedit, initial.preedit);
    ASSERT_EQ(punct.preedit_cursor, initial.preedit_cursor);
    ASSERT_EQ(punct.presentation.items.size(), initial.presentation.items.size());
    ASSERT_TRUE(!punct.ime_status.chinese_punct());
}

TEST(SessionIntegration, inline_ascii_is_returned_as_uncommitted_preedit) {
    SessionManager manager;
    manager.initialize(setup_test_dict());
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    const ProcessKeyResult initial = manager.process_key(id, make_key('I'));
    ASSERT_TRUE(initial.composing);
    ASSERT_TRUE(!initial.presentation.items.empty());

    const ProcessKeyResult plus = manager.process_key(id, make_key(VK_OEM_PLUS, true));
    ASSERT_EQ(plus.result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(plus.composing);
    ASSERT_EQ(plus.preedit, "ni+");
    ASSERT_EQ(plus.preedit_cursor, static_cast<size_t>(3));
    ASSERT_TRUE(plus.presentation.items.empty());
    ASSERT_TRUE(plus.commit_text.empty());
}

TEST(SessionIntegration, inline_ascii_binding_restores_chinese_mode_after_commit) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Shift_L"] = "inline_ascii";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    const ProcessKeyResult initial = manager.process_key(id, make_key('I'));
    ASSERT_TRUE(initial.composing);
    ASSERT_TRUE(initial.ime_status.chinese_mode());

    ASSERT_EQ(manager.process_key(id, make_key(VK_LSHIFT, true)).result,
              cxxime::ProcessResult::REJECTED);
    cxxime::KeyEvent shift_up = make_key(VK_LSHIFT);
    shift_up.is_key_up = true;
    const ProcessKeyResult converted = manager.process_key(id, shift_up);
    ASSERT_EQ(converted.result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(converted.composing);
    ASSERT_EQ(converted.preedit, "ni");
    ASSERT_TRUE(converted.presentation.items.empty());
    ASSERT_TRUE(converted.ime_status.chinese_mode());

    const ProcessKeyResult committed = manager.process_key(id, make_key(VK_RETURN));
    ASSERT_EQ(committed.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(committed.commit_text, "ni");
    ASSERT_TRUE(committed.ime_status.chinese_mode());

    const ProcessKeyResult resumed = manager.process_key(id, make_key('N'));
    ASSERT_TRUE(resumed.composing);
    ASSERT_TRUE(!resumed.presentation.items.empty());
    ASSERT_TRUE(resumed.ime_status.chinese_mode());
}

TEST(SessionIntegration, inline_ascii_binding_restores_chinese_mode_after_clear) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Shift_L"] = "inline_ascii";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key('I')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key(VK_LSHIFT, true)).result,
              cxxime::ProcessResult::REJECTED);
    cxxime::KeyEvent shift_up = make_key(VK_LSHIFT);
    shift_up.is_key_up = true;
    ASSERT_EQ(manager.process_key(id, shift_up).result, cxxime::ProcessResult::ACCEPTED);

    ASSERT_EQ(manager.clear_composition(id).status, cxxime::IPCStatus::OK);
    const ProcessKeyResult resumed = manager.process_key(id, make_key('N'));
    ASSERT_TRUE(resumed.composing);
    ASSERT_TRUE(!resumed.presentation.items.empty());
    ASSERT_TRUE(resumed.ime_status.chinese_mode());
}

TEST(SessionIntegration, inline_ascii_binding_restores_chinese_mode_after_focus_out) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Shift_L"] = "inline_ascii";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key('I')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key(VK_LSHIFT, true)).result,
              cxxime::ProcessResult::REJECTED);
    cxxime::KeyEvent shift_up = make_key(VK_LSHIFT);
    shift_up.is_key_up = true;
    ASSERT_EQ(manager.process_key(id, shift_up).result, cxxime::ProcessResult::ACCEPTED);

    ASSERT_EQ(manager.focus_out(id).status, cxxime::IPCStatus::OK);
    const ProcessKeyResult resumed = manager.process_key(id, make_key('N'));
    ASSERT_TRUE(resumed.composing);
    ASSERT_TRUE(!resumed.presentation.items.empty());
    ASSERT_TRUE(resumed.ime_status.chinese_mode());
}

TEST(SessionIntegration, clear_binding_cancels_composition_on_modifier_key_up) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Shift_L"] = "clear";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));
    const uint32_t id = manager.create_session();

    ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key('I')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(manager.process_key(id, make_key(VK_LSHIFT, true)).result,
              cxxime::ProcessResult::REJECTED);
    cxxime::KeyEvent shift_up = make_key(VK_LSHIFT);
    shift_up.is_key_up = true;
    const ProcessKeyResult cleared = manager.process_key(id, shift_up);
    ASSERT_EQ(cleared.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(cleared.result, cxxime::ProcessResult::REJECTED);
    ASSERT_TRUE(!cleared.composing);
    ASSERT_TRUE(!cleared.ime_status.chinese_mode());
}

TEST(SessionIntegration, inline_ascii_binding_restores_chinese_mode_after_deleting_preedit) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Shift_L"] = "inline_ascii";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));

    for (bool use_delete : {false, true}) {
        const uint32_t id = manager.create_session();
        ASSERT_EQ(manager.process_key(id, make_key('N')).result, cxxime::ProcessResult::ACCEPTED);
        ASSERT_EQ(manager.process_key(id, make_key(VK_LSHIFT, true)).result,
                  cxxime::ProcessResult::REJECTED);
        cxxime::KeyEvent shift_up = make_key(VK_LSHIFT);
        shift_up.is_key_up = true;
        ASSERT_EQ(manager.process_key(id, shift_up).result, cxxime::ProcessResult::ACCEPTED);

        if (use_delete) {
            ASSERT_EQ(manager.process_key(id, make_key(VK_LEFT)).result,
                      cxxime::ProcessResult::ACCEPTED);
        }
        const ProcessKeyResult cleared =
            manager.process_key(id, make_key(use_delete ? VK_DELETE : VK_BACK));
        ASSERT_TRUE(!cleared.composing);
        ASSERT_TRUE(cleared.ime_status.chinese_mode());

        const ProcessKeyResult resumed = manager.process_key(id, make_key('N'));
        ASSERT_TRUE(resumed.composing);
        ASSERT_TRUE(resumed.ime_status.chinese_mode());
        const ProcessKeyResult candidates = manager.process_key(id, make_key('I'));
        ASSERT_TRUE(candidates.composing);
        ASSERT_TRUE(!candidates.presentation.items.empty())
            << "delete=" << use_delete << " preedit=" << candidates.preedit
            << " known=" << candidates.presentation.extent.known_count;
        ASSERT_TRUE(candidates.ime_status.chinese_mode());
    }
}

static bool wait_for_count(std::atomic<int>& value, int expected, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (value.load() < expected) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

TEST(SessionIntegration, initialize_rejects_unknown_wubi_index_role) {
    const std::string dict_path = make_temp_path("test_unknown_wubi_role_dict.bin");
    create_test_dictionary_bundle(dict_path, {{"ni", "你", 100}});
    write_manifest_for_files(dict_path, {
        {"pinyin_dict", dict_path},
        {"pinyin_idx", dict_path + ".idx"},
        {"pinyin_spellings", dict_path + ".spellings.bin"},
        {"pinyin_topn", dict_path + ".topn.bin"},
        {"wubi_dict", dict_path + ".wubi.bin"},
        {"wubi_idx", dict_path + ".wubi.bin.idx"},
    });

    SessionManager manager;
    ASSERT_TRUE(!manager.initialize(dict_path));
    delete_test_dictionary_bundle(dict_path);
}

// ============================================================
// process_key tests
// ============================================================

TEST(SessionIntegration, process_key_ok) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();
    ASSERT_GT(id, (uint32_t)0);

    // Type a letter in Chinese mode
    auto r = mgr.process_key(id, make_key('N'));
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_TRUE(r.composing);
    ASSERT_EQ(r.preedit, "n");
    ASSERT_EQ(r.preedit_cursor, static_cast<size_t>(1));

    r = mgr.process_key(id, make_key(VK_LEFT));
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_TRUE(r.composing);
    ASSERT_EQ(r.preedit, "n");
    ASSERT_EQ(r.preedit_cursor, static_cast<size_t>(0));
}

TEST(SessionIntegration, wubi_fifth_key_returns_commit_and_next_composition) {
    const std::string dict_path = make_temp_path("test_wubi_fifth_key_session.bin");
    create_test_dictionary_bundle_with_wubi(dict_path, {{"a", "拼", 100}},
                                                {
                                                    {"abcd", "首选", 300},
                                                    {"abcd", "次选", 200},
                                                    {"e", "下一项", 100},
                                                });

    auto config = std::make_shared<cxxime::Config>();
    config->input_mode = static_cast<int>(cxxime::InputMode::WUBI);
    config->wubi_commit_first_on_fifth_key = true;
    SessionManager mgr;
    ASSERT_TRUE(mgr.initialize(dict_path, config));
    const uint32_t id = mgr.create_session();

    ASSERT_EQ(mgr.process_key(id, make_key('A')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(mgr.process_key(id, make_key('B')).result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(mgr.process_key(id, make_key('C')).result, cxxime::ProcessResult::ACCEPTED);
    const ProcessKeyResult fourth = mgr.process_key(id, make_key('D'));
    ASSERT_EQ(fourth.result, cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fourth.focused_preedit_start_bytes, fourth.preedit.size());
    ASSERT_EQ(fourth.focused_preedit_end_bytes, fourth.preedit.size());

    const auto result = mgr.process_key(id, make_key('E'));
    ASSERT_EQ(result.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(result.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(result.commit_text, "首选");
    ASSERT_TRUE(result.composing);
    ASSERT_EQ(result.preedit, "e");
    ASSERT_EQ(result.preedit_cursor, static_cast<size_t>(1));
    ASSERT_TRUE(candidate_contains(result.presentation, "下一项"));

    mgr.destroy_session(id);
    delete_test_dictionary_bundle(dict_path);
}

TEST(SessionIntegration, segmented_preedit_focuses_wubi_and_mixed_wubi_candidates) {
    const std::string dict_path = make_temp_path("test_wubi_preedit_focus_session.bin");
    create_test_dictionary_bundle_with_wubi(dict_path, {{"a", "拼", 100}},
                                            {{"abcd", "五笔首选", 300}, {"abcd", "五笔次选", 200}});
    constexpr uint64_t kCapabilities = cxxime::kClientCapabilitySegmentedSelection |
                                       cxxime::kClientCapabilitySegmentedPreeditPresentation;

    for (cxxime::InputMode mode : {cxxime::InputMode::WUBI, cxxime::InputMode::MIXED}) {
        auto config = std::make_shared<cxxime::Config>();
        config->input_mode = static_cast<int>(mode);
        config->mixed_candidate_preference = cxxime::MixedCandidatePreference::kWubi;
        SessionManager manager;
        ASSERT_TRUE(manager.initialize(dict_path, config));
        const uint32_t id = manager.create_session(kCapabilities);

        ProcessKeyResult result;
        for (char key : std::string("ABCD")) {
            result = manager.process_key(id, make_key(static_cast<uint32_t>(key)));
        }
        ASSERT_TRUE(result.composing);
        ASSERT_TRUE(!result.presentation.items.empty());
        ASSERT_EQ(result.preedit, "abcd");
        ASSERT_EQ(result.focused_preedit_start_bytes, static_cast<std::size_t>(0));
        ASSERT_EQ(result.focused_preedit_end_bytes, result.preedit.size());
        ASSERT_EQ(result.preedit_presentation_flags, static_cast<uint32_t>(0));
        ASSERT_EQ(manager.clear_composition(id).status, cxxime::IPCStatus::OK);
        for (char key : std::string("HSE")) {
            result = manager.process_key(id, make_key(static_cast<uint32_t>(key)));
        }
        ASSERT_TRUE(result.composing);
        ASSERT_EQ(result.preedit, "hse");
        ASSERT_TRUE(result.presentation.items.empty());
        ASSERT_EQ(result.focused_preedit_start_bytes, static_cast<std::size_t>(0));
        ASSERT_EQ(result.focused_preedit_end_bytes, result.preedit.size());
        manager.destroy_session(id);
    }

    delete_test_dictionary_bundle(dict_path);
}

TEST(SessionIntegration, process_key_invalid_session) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());

    auto r = mgr.process_key(999, make_key('N'));
    ASSERT_EQ(r.status, cxxime::IPCStatus::ERR_INVALID_SESSION);
}

TEST(SessionIntegration, process_key_committed_has_ime_status) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();

    // Toggle to English mode
    mgr.toggle_chinese(id);

    // Type a letter in English mode → should be committed
    auto r = mgr.process_key(id, make_key('A'));
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    // ime_status should be filled
    ASSERT_EQ(r.ime_status.chinese_mode(), false);
}

TEST(SessionIntegration, english_capslock_letter_preserves_engine_case) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();

    mgr.toggle_chinese(id);

    auto upper = mgr.process_key(id, make_key('N', false, true));
    ASSERT_EQ(upper.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(upper.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(upper.commit_text, "N");

    auto lower = mgr.process_key(id, make_key('N', true, true));
    ASSERT_EQ(lower.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(lower.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(lower.commit_text, "n");
}

TEST(SessionIntegration, english_capslock_keeps_english_and_outputs_uppercase) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();

    mgr.toggle_chinese(id);

    std::string text;
    for (char ch : std::string("NIHAO")) {
        auto r = mgr.process_key(id, make_key(ch, false, true));
        ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
        ASSERT_EQ(r.result, cxxime::ProcessResult::COMMITTED);
        ASSERT_TRUE(!r.composing);
        text += r.commit_text;
    }

    ASSERT_EQ(text, "NIHAO");
}

TEST(SessionIntegration, idle_space_respects_shape_in_chinese_english_and_capslock_modes) {
    const std::string dict_path = setup_test_dict();
    for (bool english : {false, true}) {
        for (bool caps : {false, true}) {
            for (bool full_shape : {false, true}) {
                auto config = std::make_shared<cxxime::Config>();
                config->ascii_switch_key["Caps_Lock"] = "clear";
                config->initial_full_shape = full_shape;
                SessionManager manager;
                ASSERT_TRUE(manager.initialize(dict_path, config));
                const uint32_t id = manager.create_session();
                if (english) {
                    manager.toggle_chinese(id);
                }

                // The first space must also synchronize an already-enabled CapsLock.
                cxxime::KeyEvent space = make_key(VK_SPACE, false, caps);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    const auto result = manager.process_key(id, space);
                    ASSERT_EQ(result.status, cxxime::IPCStatus::OK);
                    ASSERT_EQ(result.result, full_shape ? cxxime::ProcessResult::COMMITTED
                                                        : cxxime::ProcessResult::REJECTED);
                    ASSERT_EQ(result.commit_text, full_shape ? "\xE3\x80\x80" : "");
                    ASSERT_TRUE(!result.composing);
                    ASSERT_TRUE(result.preedit.empty());
                    ASSERT_EQ(result.ime_status.caps_lock(), caps);
                    ASSERT_EQ(result.ime_status.chinese_mode(), !english && !caps);
                }

                space.is_key_up = true;
                const auto released = manager.process_key(id, space);
                ASSERT_EQ(released.result, cxxime::ProcessResult::REJECTED);
                ASSERT_TRUE(released.commit_text.empty());
                ASSERT_TRUE(!released.composing);
            }
        }
    }
}

TEST(SessionIntegration, capslock_shift_space_still_toggles_shape) {
    auto config = std::make_shared<cxxime::Config>();
    config->ascii_switch_key["Caps_Lock"] = "clear";
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict(), config));
    const uint32_t id = manager.create_session();

    for (bool full_shape : {true, false}) {
        const auto result = manager.process_key(id, make_key(VK_SPACE, true, true));
        ASSERT_EQ(result.result, cxxime::ProcessResult::TOGGLE_SHAPE);
        ASSERT_EQ(result.ime_status.full_shape(), full_shape);
        ASSERT_TRUE(result.ime_status.caps_lock());
        ASSERT_TRUE(result.commit_text.empty());
        ASSERT_TRUE(!result.composing);
    }
}

TEST(SessionIntegration, english_enter_passes_to_application) {
    SessionManager mgr;
    mgr.initialize(setup_test_dict());
    uint32_t id = mgr.create_session();

    mgr.toggle_chinese(id);

    auto r = mgr.process_key(id, make_key(VK_RETURN));
    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r.result, cxxime::ProcessResult::REJECTED);
    ASSERT_TRUE(r.commit_text.empty());
    ASSERT_TRUE(!r.composing);
    ASSERT_EQ(r.ime_status.chinese_mode(), false);
}

TEST(SessionIntegration, append_enter_preserves_case_through_output_composer) {
    std::string cfg_path = make_temp_path("test_append_config.json");
    {
        std::ofstream f(cfg_path);
        f << R"({"ascii_composer":{"switch_key":{"Caps_Lock":"append"}}})";
    }

    auto config = std::make_shared<cxxime::Config>();
    ASSERT_TRUE(config->load(cfg_path));
    SessionManager mgr;
    mgr.initialize(setup_test_dict(), config);
    uint32_t id = mgr.create_session();

    mgr.process_key(id, make_key('N'));
    mgr.process_key(id, make_key('I'));

    cxxime::KeyEvent caps;
    caps.keycode = 0x14;
    caps.is_key_up = false;
    caps.set_caps_lock();
    mgr.process_key(id, caps);

    mgr.process_key(id, make_key('D', false, true));
    mgr.process_key(id, make_key('D', false, true));

    cxxime::KeyEvent enter;
    enter.keycode = 0x0D;
    enter.is_key_up = false;
    enter.set_caps_lock();
    auto r = mgr.process_key(id, enter);

    ASSERT_EQ(r.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(r.result, cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(r.commit_text, "niDD");

    DeleteFileA(cfg_path.c_str());
}

TEST(SessionIntegration, user_data_merge_updates_live_lexicon_and_skips_invalid_rows) {
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict()));

    const std::map<std::string, std::string> replacement = {
        {"user_pinyin.tsv", "备份词\tbeifenci\t100\tbei:fen:ci\n"},
    };
    merge_user_data_for_test(manager, replacement);
    const auto imported = manager.query_lexicon_entries(
        cxxime::LexiconResource::kUserLexicon, "备份词", cxxime::UserDictKind::PINYIN, 0, 16, true);
    ASSERT_EQ(imported.entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(imported.entries[0].code, "beifenci");

    const std::map<std::string, std::string> invalid_files[] = {
        {{"user_pinyin.tsv", "invalid"}},
        {{"learning_pinyin.tsv", "invalid"}},
        {{"candidate_order_pinyin.tsv", "invalid"}},
        {{"disabled_pinyin.tsv", "invalid\tentry"}},
        {{"learning_composition.tsv", "invalid"}},
    };
    for (const auto& invalid : invalid_files) {
        std::size_t imported = 0;
        std::size_t skipped = 0;
        manager.merge_user_data(invalid, &imported, &skipped);
        ASSERT_EQ(imported, static_cast<std::size_t>(0));
        ASSERT_TRUE(skipped != 0);
    }
    ASSERT_EQ(manager
                    .query_lexicon_entries(cxxime::LexiconResource::kUserLexicon, "备份词",
                                            cxxime::UserDictKind::PINYIN, 0, 16, true)
                    .entries.size(),
                static_cast<std::size_t>(1));

}

TEST(SessionIntegration, user_data_merge_skips_failed_file_and_continues) {
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict()));
    const std::string learning_path = test_user_data_dir + "\\learning_composition.tsv";
    DeleteFileA(learning_path.c_str());
    RemoveDirectoryA(learning_path.c_str());
    ASSERT_TRUE(CreateDirectoryA(learning_path.c_str(), nullptr) != FALSE);

    const std::map<std::string, std::string> imported_data = {
        {"learning_composition.tsv", "你好\tnihao\tni:hao\t2\t3\n"},
        {"user_pinyin.tsv", "继续导入\tjixudaoru\t5\tji:xu:dao:ru\n"},
    };
    std::size_t imported = 0;
    std::size_t skipped = 0;
    manager.merge_user_data(imported_data, &imported, &skipped);
    ASSERT_EQ(imported, static_cast<std::size_t>(1));
    ASSERT_TRUE(skipped != 0);
    ASSERT_EQ(manager
                  .query_lexicon_entries(cxxime::LexiconResource::kUserLexicon, "继续导入",
                                         cxxime::UserDictKind::PINYIN, 0, 16, true)
                  .entries.size(),
              static_cast<std::size_t>(1));

    ASSERT_TRUE(RemoveDirectoryA(learning_path.c_str()) != FALSE);
    manager.merge_user_data({{"learning_composition.tsv", "你好\tnihao\tni:hao\t2\t3\n"}},
                            &imported, &skipped);
    ASSERT_EQ(imported, static_cast<std::size_t>(1));
    ASSERT_EQ(skipped, static_cast<std::size_t>(0));
    DeleteFileA(learning_path.c_str());
}

int main() {
    GetTempPathA(MAX_PATH, temp_path);
    const std::string directory_name =
        "cxxime-session-integration-" + std::to_string(GetCurrentProcessId());
    test_user_data_dir = make_temp_path(directory_name.c_str());
    CreateDirectoryA(test_user_data_dir.c_str(), nullptr);
    DeleteFileA((test_user_data_dir + "\\default.json").c_str());
    DeleteFileA((test_user_data_dir + "\\user_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\user_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\disabled_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\disabled_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\candidate_order_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\candidate_order_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_composition.tsv").c_str());

    cxxime::set_data_dir(CXXIME_DATA_DIR);
    cxxime::set_user_data_dir(test_user_data_dir);
    const int result = test::RunAllTests();

    DeleteFileA((test_user_data_dir + "\\default.json").c_str());
    DeleteFileA((test_user_data_dir + "\\user_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\user_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\disabled_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\disabled_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\candidate_order_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\candidate_order_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_pinyin.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_wubi.tsv").c_str());
    DeleteFileA((test_user_data_dir + "\\learning_composition.tsv").c_str());
    RemoveDirectoryA(test_user_data_dir.c_str());
    return result;
}
