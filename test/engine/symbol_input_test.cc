// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdio>
#include <memory>
#include <string>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/dict.h>
#include <cxxime/engine.h>
#include <cxxime/output_options.h>
#include <cxxime/spellings_index.h>
#include <cxxime/symbol_table.h>

#include "support/testutil.h"
#include "support/test_runtime.h"

namespace {

cxxime::KeyEvent make_key(uint32_t keycode, bool key_up = false) {
    cxxime::KeyEvent event;
    event.keycode = keycode;
    event.is_key_up = key_up;
    return event;
}

cxxime::KeyEvent make_shift_key(uint32_t keycode) {
    cxxime::KeyEvent event = make_key(keycode);
    event.set_shift();
    return event;
}

class SymbolEngineFixture {
public:
    bool initialize() {
        char temp_path[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, temp_path) == 0) {
            return false;
        }
        dict_path_ = std::string(temp_path) + "zhiyi_symbol_input.bin";
        user_path_ = std::string(temp_path) + "zhiyi_symbol_input.tsv";
        DeleteFileA(dict_path_.c_str());
        DeleteFileA(user_path_.c_str());

        if (!cxxime::Dict::create_test_dict(
                dict_path_, {{"a", "first", 100}, {"a", "second", 90}}) ||
            !dict_->open(dict_path_, user_path_) ||
            !wubi_dict_->open_dict(dict_path_) ||
            !symbols_->load(std::string(CXXIME_DATA_DIR) + "symbols.json")) {
            return false;
        }

        config_.candidate_learning = true;
        config_.wubi_auto_commit = true;
        config_.wubi_commit_first_on_fifth_key = true;
        config_.ascii_switch_key["Shift_L"] = "code";
        if (!test::initialize_engine(engine_, dict_, config_, {}, wubi_dict_, symbols_)) {
            return false;
        }
        engine_.set_trace_enabled(false);
        return true;
    }

    ~SymbolEngineFixture() {
        engine_.finalize();
        dict_->close();
        wubi_dict_->close();
        DeleteFileA(dict_path_.c_str());
        DeleteFileA(user_path_.c_str());
    }

    cxxime::Engine& engine() { return engine_; }

private:
    std::string dict_path_;
    std::string user_path_;
    std::shared_ptr<cxxime::Dict> dict_ = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    std::shared_ptr<cxxime::Dict> wubi_dict_ = std::make_shared<cxxime::Dict>(cxxime::UserDictKind::WUBI);
    cxxime::Config config_;
    std::shared_ptr<cxxime::SymbolTable> symbols_ = std::make_shared<cxxime::SymbolTable>();
    cxxime::Engine engine_;
};

void type_symbol_code(cxxime::Engine& engine, const char* code,
                      const cxxime::OutputOptions& options = {}) {
    ASSERT_EQ(engine.process_key(make_key(VK_OEM_5), options), cxxime::ProcessResult::ACCEPTED);
    for (const char* ch = code; *ch; ++ch) {
        ASSERT_EQ(engine.process_key(make_key(static_cast<uint32_t>(*ch - 'a' + 'A')), options),
                  cxxime::ProcessResult::ACCEPTED);
    }
}

cxxime::PunctMapping make_enumeration_punctuation() {
    cxxime::PunctMapping punctuation;
    punctuation.half_shape["/"] = {cxxime::PunctType::COMMIT, u8"、", {}, {}};
    punctuation.half_shape["\\"] = {cxxime::PunctType::COMMIT, u8"、", {}, {}};
    return punctuation;
}

} // namespace

TEST(SymbolInput, works_in_all_input_modes) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());

    const cxxime::InputMode modes[] = {
        cxxime::InputMode::PINYIN,
        cxxime::InputMode::WUBI,
        cxxime::InputMode::MIXED,
    };
    for (cxxime::InputMode mode : modes) {
        fixture.engine().clear();
        fixture.engine().switch_mode(mode);
        type_symbol_code(fixture.engine(), "bd");
        ASSERT_EQ(fixture.engine().context().active_input(), "\\bd");
        ASSERT_EQ(fixture.engine().context().composition_scheme(),
                  cxxime::CompositionScheme::kSymbol);
        ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());
        ASSERT_EQ(fixture.engine().context().candidate_page().candidates[0].source,
                  cxxime::CandidateSource::kSymbol);
    }
}

TEST(SymbolInput, every_published_symbol_can_be_paged_and_committed_in_all_modes) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    cxxime::SymbolTable table;
    ASSERT_TRUE(table.load(std::string(CXXIME_DATA_DIR) + "symbols.json"));
    const cxxime::InputMode modes[] = {
        cxxime::InputMode::PINYIN, cxxime::InputMode::WUBI, cxxime::InputMode::MIXED,
    };
    for (const auto mode : modes) {
        fixture.engine().switch_mode(mode);
        int committed_count = 0;
        for (int category_page = 0;; ++category_page) {
            const auto categories = table.translate_page("", category_page, 9);
            for (const auto& category : categories.candidates) {
                const std::string code = category.code.substr(1);
                fixture.engine().clear();
                type_symbol_code(fixture.engine(), code.c_str());
                const int page_size = fixture.engine().context().candidate_page().page_size;
                for (int page = 0;; ++page) {
                    const auto expected = table.translate_page(code, page, page_size);
                    for (size_t i = 0; i < expected.candidates.size(); ++i) {
                        fixture.engine().clear();
                        type_symbol_code(fixture.engine(), code.c_str());
                        for (int step = 0; step < page; ++step) {
                            ASSERT_EQ(fixture.engine().process_key(make_key(VK_NEXT)),
                                      cxxime::ProcessResult::ACCEPTED);
                        }
                        ASSERT_EQ(fixture.engine().process_key(make_key('1' + static_cast<int>(i))),
                                  cxxime::ProcessResult::COMMITTED);
                        const auto commit = fixture.engine().take_commit_text_with_source();
                        ASSERT_EQ(commit.first, expected.candidates[i].text);
                        ASSERT_EQ(commit.second, cxxime::CommitSource::kCandidate);
                        ++committed_count;
                    }
                    if (expected.extent.state != cxxime::CandidateExtentState::kHasMore) {
                        break;
                    }
                }
            }
            if (categories.extent.state != cxxime::CandidateExtentState::kHasMore) {
                break;
            }
        }
        ASSERT_EQ(committed_count, 304);
    }
}

TEST(SymbolInput, enter_commits_raw_trigger) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "");

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_RETURN)), cxxime::ProcessResult::COMMITTED);
    const auto committed = fixture.engine().take_commit_text_with_source();
    ASSERT_EQ(committed.first, "\\");
    ASSERT_EQ(committed.second, cxxime::CommitSource::kRawCodePreserveCase);
}

TEST(SymbolInput, bare_trigger_navigates_categories_without_committing) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "");

    const auto categories = fixture.engine().context().candidate_page();
    ASSERT_EQ(categories.extent.known_count, 10);
    ASSERT_EQ(categories.extent.state, cxxime::CandidateExtentState::kHasMore);
    ASSERT_EQ(categories.candidates[0].text, "补充标点");
    ASSERT_EQ(categories.candidates[0].comment, "\\bd");

    ASSERT_EQ(fixture.engine().process_key(make_key('1')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd");
    ASSERT_TRUE(fixture.engine().context().committed_text.empty());
    ASSERT_EQ(fixture.engine().context().candidate_page().candidates[0].text, u8"\u2026");

    fixture.engine().clear();
    type_symbol_code(fixture.engine(), "");
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_SPACE)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd");
}

TEST(SymbolInput, mouse_selection_navigates_categories_without_committing) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "");

    ASSERT_TRUE(fixture.engine().select_candidate(0));
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd");
    ASSERT_TRUE(fixture.engine().context().committed_text.empty());
    ASSERT_EQ(fixture.engine().context().candidate_page().candidates[0].text, u8"\u2026");
}

TEST(SymbolInput, shift_commits_raw_trigger_and_switches_to_english) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "");

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_LSHIFT)), cxxime::ProcessResult::REJECTED);
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_LSHIFT, true)),
              cxxime::ProcessResult::COMMITTED);
    const auto committed = fixture.engine().take_commit_text_with_source();
    ASSERT_EQ(committed.first, "\\");
    ASSERT_TRUE(fixture.engine().ascii_composer().is_ascii_mode());
}

TEST(SymbolInput, disabled_state_passes_trigger_to_application) {
    const uint32_t keys[] = {VK_OEM_2, VK_OEM_5};
    for (uint32_t key : keys) {
        SymbolEngineFixture fixture;
        ASSERT_TRUE(fixture.initialize());

        cxxime::OutputOptions options;
        options.chinese_punct = false;
        ASSERT_EQ(fixture.engine().process_key(make_key(key), options),
                  cxxime::ProcessResult::REJECTED);
        ASSERT_TRUE(!fixture.engine().context().is_composing());

        options.chinese_mode = false;
        options.chinese_punct = true;
        ASSERT_EQ(fixture.engine().process_key(make_key(key), options),
                  cxxime::ProcessResult::REJECTED);
        ASSERT_TRUE(!fixture.engine().context().is_composing());
    }
}

TEST(SymbolInput, slash_commits_enumeration_comma_while_idle) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    const cxxime::PunctMapping punctuation = make_enumeration_punctuation();
    cxxime::OutputOptions options;
    options.punct_mapping = &punctuation;

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_OEM_2), options),
              cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(fixture.engine().take_commit_text_with_source().first, u8"、");
    ASSERT_TRUE(!fixture.engine().context().is_composing());
}

TEST(SymbolInput, backslash_starts_symbol_input_before_idle_punctuation) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    const cxxime::PunctMapping punctuation = make_enumeration_punctuation();
    cxxime::OutputOptions options;
    options.punct_mapping = &punctuation;

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_OEM_5), options),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\");
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kSymbol);
    ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());
}

TEST(SymbolInput, main_symbols_use_full_shape_while_idle) {
    const struct {
        uint32_t key;
        bool shift;
        const char* full_width;
    } cases[] = {
        {VK_OEM_PERIOD, false, u8"．"},
        {VK_OEM_2, false, u8"／"},
        {VK_OEM_5, false, u8"＼"},
        {VK_OEM_MINUS, false, u8"－"},
        {VK_OEM_4, false, u8"［"},
        {VK_OEM_COMMA, true, u8"＜"},
        {VK_OEM_7, false, u8"＇"},
        {VK_OEM_7, true, u8"＂"},
        {'1', true, u8"！"},
    };
    for (const auto& test_case : cases) {
        SymbolEngineFixture fixture;
        ASSERT_TRUE(fixture.initialize());
        const cxxime::PunctMapping punctuation = make_enumeration_punctuation();
        cxxime::OutputOptions options;
        options.chinese_punct = true;
        options.full_shape = true;
        options.punct_mapping = &punctuation;
        const cxxime::KeyEvent event =
            test_case.shift ? make_shift_key(test_case.key) : make_key(test_case.key);

        ASSERT_EQ(fixture.engine().process_key(event, options),
                  cxxime::ProcessResult::COMMITTED);
        ASSERT_EQ(fixture.engine().take_commit_text_with_source().first, test_case.full_width);
        ASSERT_TRUE(!fixture.engine().context().is_composing());
    }
}

TEST(SymbolInput, candidate_pagination_keys_remain_commands_in_full_shape) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    cxxime::OutputOptions options;
    options.full_shape = true;

    ASSERT_EQ(fixture.engine().process_key(make_key('A'), options),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_OEM_PLUS), options),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_OEM_MINUS), options),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "a");
    ASSERT_TRUE(fixture.engine().context().committed_text.empty());
}

TEST(SymbolInput, slash_and_backslash_commit_ime_candidate_with_enumeration_comma) {
    const uint32_t keys[] = {VK_OEM_2, VK_OEM_5};
    for (uint32_t key : keys) {
        SymbolEngineFixture fixture;
        ASSERT_TRUE(fixture.initialize());

        const cxxime::PunctMapping punctuation = make_enumeration_punctuation();
        cxxime::OutputOptions options;
        options.punct_mapping = &punctuation;

        ASSERT_EQ(fixture.engine().process_key(make_key('A'), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_GE(fixture.engine().context().candidate_page().candidates.size(), 2u);
        fixture.engine().context().translation().highlighted = 1;
        const std::string expected =
            fixture.engine().context().candidate_page().candidates[1].text + u8"、";
        ASSERT_EQ(fixture.engine().process_key(make_key(key), options),
                  cxxime::ProcessResult::COMMITTED);
        const auto committed = fixture.engine().take_commit_text_with_source();
        ASSERT_EQ(committed.first, expected);
        ASSERT_EQ(committed.second, cxxime::CommitSource::kCandidate);
    }
}

TEST(SymbolInput, slash_and_backslash_extend_ime_preedit_without_candidates) {
    const struct {
        uint32_t key;
        const char* expected;
    } cases[] = {
        {VK_OEM_2, "z/"},
        {VK_OEM_5, "z\\"},
    };
    for (const auto& test_case : cases) {
        SymbolEngineFixture fixture;
        ASSERT_TRUE(fixture.initialize());
        const cxxime::PunctMapping punctuation = make_enumeration_punctuation();
        cxxime::OutputOptions options;
        options.punct_mapping = &punctuation;

        ASSERT_EQ(fixture.engine().process_key(make_key('Z'), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_TRUE(fixture.engine().context().candidate_page().candidates.empty());
        ASSERT_EQ(fixture.engine().process_key(make_key(test_case.key), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_EQ(fixture.engine().context().active_input(), test_case.expected);
        ASSERT_EQ(fixture.engine().context().composition_scheme(),
                  cxxime::CompositionScheme::kInlineAscii);
        ASSERT_TRUE(fixture.engine().context().committed_text.empty());
    }
}

TEST(SymbolInput, slash_and_backslash_preserve_full_shape_composition_behavior) {
    const struct {
        uint32_t key;
        const char* full_width;
    } cases[] = {
        {VK_OEM_2, u8"／"},
        {VK_OEM_5, u8"＼"},
    };
    for (const auto& test_case : cases) {
        SymbolEngineFixture candidate_fixture;
        ASSERT_TRUE(candidate_fixture.initialize());
        cxxime::OutputOptions options;
        options.full_shape = true;

        ASSERT_EQ(candidate_fixture.engine().process_key(make_key('A'), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_GE(candidate_fixture.engine().context().candidate_page().candidates.size(), 2u);
        candidate_fixture.engine().context().translation().highlighted = 1;
        const std::string expected_candidate =
            candidate_fixture.engine().context().candidate_page().candidates[1].text +
            test_case.full_width;
        ASSERT_EQ(candidate_fixture.engine().process_key(make_key(test_case.key), options),
                  cxxime::ProcessResult::COMMITTED);
        ASSERT_EQ(candidate_fixture.engine().take_commit_text_with_source().first,
                  expected_candidate);

        SymbolEngineFixture raw_fixture;
        ASSERT_TRUE(raw_fixture.initialize());
        ASSERT_EQ(raw_fixture.engine().process_key(make_key('Z'), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_TRUE(raw_fixture.engine().context().candidate_page().candidates.empty());
        ASSERT_EQ(raw_fixture.engine().process_key(make_key(test_case.key), options),
                  cxxime::ProcessResult::COMMITTED);
        ASSERT_EQ(raw_fixture.engine().take_commit_text_with_source().first,
                  std::string("z") + test_case.full_width);
    }
}

TEST(SymbolInput, numpad_operator_stays_ascii_in_full_shape_composition) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());

    type_symbol_code(fixture.engine(), "bd");
    ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());

    cxxime::OutputOptions options;
    options.full_shape = true;

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_ADD), options),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd+");
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kInlineAscii);
    ASSERT_TRUE(fixture.engine().context().candidate_page().candidates.empty());
    ASSERT_TRUE(fixture.engine().context().committed_text.empty());
}

TEST(SymbolInput, slash_and_backslash_finish_composition_with_english_punctuation) {
    const struct {
        uint32_t key;
        const char* text;
    } cases[] = {
        {VK_OEM_2, "/"},
        {VK_OEM_5, "\\"},
    };
    for (const auto& test_case : cases) {
        SymbolEngineFixture fixture;
        ASSERT_TRUE(fixture.initialize());
        cxxime::OutputOptions options;
        options.chinese_punct = false;

        ASSERT_EQ(fixture.engine().process_key(make_key('A'), options),
                  cxxime::ProcessResult::ACCEPTED);
        ASSERT_GE(fixture.engine().context().candidate_page().candidates.size(), 2u);
        fixture.engine().context().translation().highlighted = 1;
        const std::string expected =
            fixture.engine().context().candidate_page().candidates[1].text + test_case.text;
        ASSERT_EQ(fixture.engine().process_key(make_key(test_case.key), options),
                  cxxime::ProcessResult::COMMITTED);
        ASSERT_EQ(fixture.engine().take_commit_text_with_source().first, expected);
        ASSERT_TRUE(!fixture.engine().context().is_composing());
    }
}

TEST(SymbolInput, rejected_printable_restores_symbol_origin_after_delete) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "bd");
    ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());

    ASSERT_EQ(fixture.engine().process_key(make_shift_key(VK_OEM_PLUS)),
              cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd+");
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kInlineAscii);
    ASSERT_TRUE(fixture.engine().context().candidate_page().candidates.empty());

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_BACK)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bd");
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kSymbol);
    ASSERT_TRUE(!fixture.engine().context().candidate_page().candidates.empty());
}

TEST(SymbolInput, shifted_digit_does_not_select_candidate) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "bd");
    const int highlighted = fixture.engine().context().translation().highlighted;
    ASSERT_GE(highlighted, 0);
    const std::string expected =
        fixture.engine().context().candidate_page().candidates[highlighted].text + "!";

    ASSERT_EQ(fixture.engine().process_key(make_shift_key('1')), cxxime::ProcessResult::COMMITTED);
    const auto committed = fixture.engine().take_commit_text_with_source();
    ASSERT_EQ(committed.first, expected);
    ASSERT_EQ(committed.second, cxxime::CommitSource::kCandidate);
}

TEST(SymbolInput, shifted_zero_commits_candidate_with_right_parenthesis) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "bd");
    const int highlighted = fixture.engine().context().translation().highlighted;
    ASSERT_GE(highlighted, 0);
    const std::string expected =
        fixture.engine().context().candidate_page().candidates[highlighted].text + u8"）";
    cxxime::PunctMapping punctuation;
    punctuation.half_shape[")"] = {cxxime::PunctType::COMMIT, u8"）", {}, {}};
    cxxime::OutputOptions options;
    options.punct_mapping = &punctuation;

    ASSERT_EQ(fixture.engine().process_key(make_shift_key('0'), options),
              cxxime::ProcessResult::COMMITTED);
    ASSERT_EQ(fixture.engine().take_commit_text_with_source().first, expected);
    ASSERT_TRUE(!fixture.engine().context().is_composing());
}

TEST(SymbolInput, supports_navigation_pagination_editing_and_cancel) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    type_symbol_code(fixture.engine(), "sz");

    const std::string first = fixture.engine().context().candidate_page().candidates[0].text;
    const int page_size =
        static_cast<int>(fixture.engine().context().candidate_page().candidates.size());
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_DOWN)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().translation().highlighted, 1);
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_UP)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().translation().highlighted, 0);
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_NEXT)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().page_offset(), page_size);
    ASSERT_NE(fixture.engine().context().candidate_page().candidates[0].text, first);

    ASSERT_EQ(fixture.engine().process_key(make_key(VK_BACK)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\s");
    ASSERT_TRUE(fixture.engine().context().candidate_page().candidates.empty());
    ASSERT_EQ(fixture.engine().process_key(make_key(VK_ESCAPE)), cxxime::ProcessResult::ACCEPTED);
    ASSERT_TRUE(!fixture.engine().context().is_composing());
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kPinyin);
    ASSERT_EQ(fixture.engine().process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().composition_scheme(),
              cxxime::CompositionScheme::kPinyin);
    ASSERT_EQ(fixture.engine().context().active_input(), "a");
}

TEST(SymbolInput, ignores_wubi_auto_commit_rules) {
    SymbolEngineFixture fixture;
    ASSERT_TRUE(fixture.initialize());
    fixture.engine().switch_mode(cxxime::InputMode::WUBI);
    type_symbol_code(fixture.engine(), "bd");

    ASSERT_EQ(fixture.engine().process_key(make_key('X')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().process_key(make_key('A')), cxxime::ProcessResult::ACCEPTED);
    ASSERT_EQ(fixture.engine().context().active_input(), "\\bdxa");
    ASSERT_TRUE(fixture.engine().context().committed_text.empty());
}

RUN_ALL_TESTS()
