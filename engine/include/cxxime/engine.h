// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_ENGINE_H_
#define CXXIME_ENGINE_H_

#include <optional>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include <cxxime/ascii_composer.h>
#include <cxxime/composition_learning.h>
#include <cxxime/composition_presentation.h>
#include <cxxime/config.h>
#include <cxxime/context.h>
#include <cxxime/dict.h>
#include <cxxime/engine_runtime.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/output_options.h>
#include <cxxime/processor.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_scratch.h>
#include <cxxime/query_trace.h>
#include <cxxime/symbol_processor.h>
#include <cxxime/translator.h>

namespace cxxime {

class SymbolTable;

class Engine {
public:
    // Self-contained Pinyin init: owns resources loaded from the supplied paths.
    bool initialize(const std::string& dict_path, const std::string& config_path = "");

    bool initialize(std::shared_ptr<const EngineRuntimeState> runtime);
    bool apply_runtime_state(std::shared_ptr<const EngineRuntimeState> runtime);

    void finalize();

    ProcessResult process_key(const KeyEvent& event);
    ProcessResult process_key(const KeyEvent& event, const OutputOptions& opts,
                              int visible_candidate_count = 0);
    const Context& context() const;
    Context& context();
    bool select_candidate(int index);
    void set_partial_selection_enabled(bool enabled) {
        translation_policy_.allow_partial_selection = enabled;
    }
    std::string get_commit_text();
    std::pair<std::string, CommitSource> take_commit_text_with_source();
    std::pair<std::string, CommitSource> commit_composition_with_source();
    std::string commit_raw_composition();
    void clear();
    void clear_composition();  // Clear composing state without clearing query results.

    const AsciiComposer& ascii_composer() const { return ascii_composer_; }
    AsciiComposer& ascii_composer() { return ascii_composer_; }

    // Query trace access
    const QueryTrace& last_trace() const { return trace_; }
    bool has_short_cache() const {
        return runtime_ && runtime_->pinyin_dict().has_short_cache();
    }
    void set_trace_enabled(bool enabled) { trace_enabled_ = enabled; }
    void set_trace_session_id(uint32_t id) { trace_.session_id = id; }
    void set_sentence_composition_enabled(bool enabled);
    bool sentence_composition_enabled() const { return sentence_composition_enabled_; }
    void clear_query_cache();
    CandidatePage translate_for_search(const std::string& input, int limit = 10);
    bool record_search_result(const std::string& input, const std::string& result);

    // Query budget (scan limits); deadline is set separately via set_query_deadline_ms().
    void set_query_budget(const QueryBudget& budget) { budget_ = budget; }
    const QueryBudget& query_budget() const { return budget_; }

    // Query deadline protection
    void set_query_deadline_ms(uint32_t deadline_ms) { query_deadline_ms_ = deadline_ms; }
    uint32_t query_deadline_ms() const { return query_deadline_ms_; }

    // Mode switching
    void switch_mode(InputMode mode);
    InputMode mode() const { return mode_; }

    // English mode style: word completion (true) or letter-by-letter input (false).
    void set_english_word_mode(bool enabled) { english_word_mode_ = enabled; }
    bool english_word_mode() const { return english_word_mode_; }

    static std::string derive_spellings_path(const std::string& dict_path);

private:
    void init_per_session(const Config& config);
    void reset_composition_state();
    void rebuild_pipeline(InputMode mode, bool force = false);
    bool symbol_input_enabled(const OutputOptions& opts) const;
    TranslationResult translate_current_composition(const QueryDeadline& deadline);
    TranslationResult translate_composition(const CompositionState& state,
                                            int page_index,
                                            int page_offset,
                                            const QueryDeadline& deadline,
                                            int candidate_limit = 0,
                                            uint32_t continuation_effort = 0);
    TranslationResult translate_after_visible_anchor(const QueryDeadline& deadline,
                                                     int visible_count,
                                                     int target_offset,
                                                     uint32_t continuation_effort,
                                                     bool* sequence_invalidated);
    bool dispatch_candidate_selection(int index, const QueryDeadline& deadline);
    bool finalize_selection(const CandidateEntry& entry);
    bool replace_active_input(const ReplaceActiveInputAction& action,
                              const QueryDeadline& deadline);
    void apply_commit_learning_plan();
    static CompositionScheme scheme_for_mode(InputMode mode);

    // Laya reranking: committed text of this session (context for the model) and the text
    // already confirmed inside the current composition.
    void remember_commit(const std::string& text);
    std::string laya_context(const CompositionState& state) const;
    std::string laya_history_;
    // English words mixed into the first pinyin page (config.english.mixed_in_chinese).
    void add_english_candidates(const std::string& input, int page_size, TranslationResult& result) const;
    // English (ASCII) mode word completion (config.english.completion_in_ascii): letters build
    // a word, candidates = the typed text + completions; 1-9 select, Space commits the
    // highlighted candidate plus a space, Enter commits the typed text.
    std::optional<ProcessResult> process_english_key(const KeyEvent& event, const OutputOptions& opts);
    void refresh_english_candidates();
    ProcessResult commit_english(std::string text);
    bool english_composing_ = false;
    bool english_word_mode_ = true;
    KeyboardShortcut english_style_shortcut_;

    std::shared_ptr<const EngineRuntimeState> runtime_;

    std::unique_ptr<IProcessor> processor_;
    std::unique_ptr<ITranslator> translator_;

    Context context_;
    AsciiComposer ascii_composer_;
    SymbolProcessor symbol_processor_;

    // Input mode
    InputMode mode_ = InputMode::PINYIN;
    TranslationPolicy translation_policy_;
    KeyboardShortcut input_mode_switch_shortcut_;
    uint32_t handled_shortcut_key_ = 0;

    // Query trace (explicit ownership, not thread_local - see TraceContext constraints)
    QueryTrace trace_;
    bool trace_enabled_ = true;
    static std::atomic<uint64_t> next_query_id_;

    // Query budget (scan limits only); deadline is per-query via QueryDeadline.
    QueryBudget budget_;
    uint32_t query_deadline_ms_ = 30;  // default 30ms deadline
    bool sentence_composition_enabled_ = true;

    // Per-engine reusable scratch buffer for translate() queries
    QueryScratch scratch_;

    // Punctuation handling
    bool handle_punctuation(const KeyEvent& event, Context& context, const OutputOptions& opts);
    bool handle_full_shape(const KeyEvent& event, Context& context, const OutputOptions& opts);
    void commit_with_punctuation(Context& context, const std::string& output);
};

} // namespace cxxime

#endif // CXXIME_ENGINE_H_
