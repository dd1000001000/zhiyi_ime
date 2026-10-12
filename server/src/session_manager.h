// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SESSION_MANAGER_H_
#define CXXIME_SESSION_MANAGER_H_

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <cxxime/experience_log.h>
#include <utility>
#include <vector>

#include <cxxime/candidate_presentation.h>
#include <cxxime/config.h>
#include <cxxime/engine.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/output_composer.h>
#include <cxxime/punct_types.h>
#include <cxxime/symbol_table.h>
#include <cxxime/user_dict.h>

struct SharedResourceSnapshot {
    std::shared_ptr<const cxxime::EngineRuntimeState> runtime;
    std::shared_ptr<const cxxime::PunctMapping> punct_mapping;
};

// Replaceable resources shared across all sessions.
struct SharedResources {
private:
    // Caller holds mutex so the selected runtime cannot change during the operation.
    std::shared_ptr<cxxime::Dict> dict_for_kind_locked(cxxime::UserDictKind kind) const;

    struct PendingRuntime {
        std::shared_ptr<const cxxime::EngineRuntimeState> runtime;
        std::shared_ptr<const cxxime::Config> requested_config;
        std::uint64_t base_generation = 0;
    };

    std::shared_ptr<const cxxime::EngineRuntimeState> runtime;
    std::shared_ptr<const cxxime::PunctMapping> punct_mapping;
    std::string punct_path;   // Stored for reload
    std::string dict_path;    // Stored for dictionary reload
    std::string wubi_dict_path;
    std::string manifest_path;
    std::optional<PendingRuntime> pending_runtime;
    std::uint64_t generation = 0;
    mutable std::mutex mutex;

public:
    bool load(const std::string& dict_path,
        const std::shared_ptr<const cxxime::Config>& config);
    SharedResourceSnapshot snapshot() const;
    std::shared_ptr<cxxime::Dict> dict_for_kind(cxxime::UserDictKind kind) const;
    bool load_punctuation(const std::string& path);
    bool prepare_config(const std::shared_ptr<const cxxime::Config>& next_config);
    bool commit_prepared_config(const std::shared_ptr<const cxxime::Config>& next_config);
    void cancel_prepared_config();
    bool reload_dictionaries();
    cxxime::IPCStatus add_user_entry(cxxime::UserDictKind kind,
        const std::string& text, const std::string& code,
        const std::string& syllables = {});
    cxxime::UserDictQueryResult query_user_entries(const std::string& query,
                                                   cxxime::UserDictKind kind,
                                                   size_t offset, size_t limit);
    cxxime::UserDictQueryResult query_lexicon_entries(
        cxxime::LexiconResource resource, const std::string& query,
        cxxime::UserDictKind kind, size_t offset, size_t limit, bool exact_text = false);
    cxxime::UserDictQueryResult query_disabled_system_entry_status(
        cxxime::UserDictKind kind, const std::vector<std::string>& texts);
    cxxime::IPCStatus delete_user_entries(
        cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries);
    cxxime::IPCStatus replace_user_entry(cxxime::UserDictKind kind,
                                         const std::string& old_text,
                                         const std::string& old_code,
                                         const std::string& new_text,
                                         const std::string& new_code,
                                         const std::string& syllables = {});
    cxxime::IPCStatus import_user_dict(cxxime::UserDictKind kind,
                                       const std::string& source_path);
    cxxime::IPCStatus save_user_dict(cxxime::UserDictKind kind);
    cxxime::IPCStatus disable_system_entry(cxxime::UserDictKind kind,
                                           const std::string& text);
    cxxime::IPCStatus restore_system_entry(cxxime::UserDictKind kind,
                                           const std::string& text);
    cxxime::IPCStatus delete_candidate_preferences(
        cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries);
    cxxime::IPCStatus clear_candidate_preferences(cxxime::UserDictKind kind);
    cxxime::IPCStatus save_candidate_preferences(cxxime::UserDictKind kind);
    cxxime::CandidateOrderQueryResult query_candidate_order(cxxime::UserDictKind kind,
                                                            const std::string& code,
                                                            std::size_t limit);
    cxxime::IPCStatus replace_candidate_order(
        cxxime::UserDictKind kind, const std::string& code,
        const std::vector<cxxime::ManualCandidateOrderEntry>& entries,
        std::uint64_t expected_version, bool* version_conflict);
    cxxime::IPCStatus clear_candidate_order(cxxime::UserDictKind kind,
                                            const std::string& code,
                                            std::uint64_t expected_version,
                                            bool* version_conflict);
    bool save_candidate_preferences(bool force);
    bool close_english_learning();
};

struct SessionEntry {
    std::unique_ptr<cxxime::Engine> engine;
    std::chrono::steady_clock::time_point last_activity;
    cxxime::ImeStatus ime_status;
    bool base_chinese_mode = true;
    bool full_shape = false;
    bool chinese_punct = true;
    bool closing = false;
    uint64_t client_capabilities = 0;
    uint64_t candidate_revision = 0;
    // The composition (Engine::continuation_key) whose continuation scores are in: its page is
    // composed again when the client fetches it (REFRESH_CANDIDATES), not before.
    std::string continuation_ready;
    SharedResourceSnapshot resources;
    std::mutex mutex;  // per-session concurrency protection
    // Input collection (second tier of the experience program): the input target reported by
    // the client and the input being typed.
    std::string input_app;
    std::string input_window_title;
    cxxime::InputRecord input;
    std::chrono::steady_clock::time_point input_start;
    bool input_open = false;
};

void apply_resource_snapshot(SessionEntry& entry, const SharedResourceSnapshot& resources);

struct ProcessKeyResult {
    cxxime::IPCStatus status = cxxime::IPCStatus::ERR_INVALID_SESSION;
    cxxime::ProcessResult result = cxxime::ProcessResult::REJECTED;
    std::string commit_text;
    bool composing = false;
    std::string preedit;
    size_t preedit_cursor = 0;
    size_t converted_prefix_bytes = 0;
    size_t focused_preedit_start_bytes = 0;
    size_t focused_preedit_end_bytes = 0;
    uint32_t preedit_presentation_flags = 0;
    cxxime::CandidatePresentationPage presentation;
    uint64_t candidate_revision = 0;
    cxxime::ImeStatus ime_status;
    // The voice key or Esc while recording (Engine::take_voice_request).
    cxxime::Engine::VoiceRequest voice_request = cxxime::Engine::VoiceRequest::kNone;
};

class ContinuationWorker;

class SessionManager {
public:
    using ConfigPatchHandler = std::function<void(const std::string& merge_patch_json)>;
    // How to tell the client showing session `session_id` that its candidates changed; empty
    // when no client shows it (its page then stays as the key left it).
    using RefreshNotifier = std::function<std::function<void()>(uint32_t session_id)>;

    SessionManager();
    ~SessionManager();

    bool initialize(const std::string& dict_path,
                    const std::shared_ptr<const cxxime::Config>& config =
                    std::make_shared<const cxxime::Config>());
    uint32_t create_session(uint64_t client_capabilities = 0);
    void destroy_session(uint32_t id);

    size_t cleanup_idle_sessions(uint32_t timeout_ms);
    bool prepare_config(const std::shared_ptr<const cxxime::Config>& config,
                        unsigned long* error_code = nullptr);
    void cancel_prepared_config();
    bool apply_config(const std::shared_ptr<const cxxime::Config>& config);
    void set_config_patch_handler(ConfigPatchHandler handler);
    cxxime::IPCStatus reload_dictionaries();
    bool reload_punctuation(const std::string& path);

    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> get_ime_status(uint32_t id);
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> toggle_chinese(uint32_t id);
    ProcessKeyResult set_chinese_mode(uint32_t id, bool chinese_mode);
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> toggle_shape(uint32_t id);
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> toggle_punct(uint32_t id);
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> switch_input_mode(uint32_t id, cxxime::InputMode mode);
    // Pinyin style (english_style false: initials or full pinyin) or English style (true: word
    // completion), shared by all sessions and saved like the style shortcut.
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> set_input_style(uint32_t id,
                                                                   bool english_style,
                                                                   bool value);
    // Pinyin style (full pinyin, initials or shuangpin), shared and saved as above.
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> set_pinyin_style(uint32_t id,
                                                                    cxxime::PinyinStyle style);
    cxxime::IPCStatus sync_ascii_mode(uint32_t id, bool ascii_mode);
    std::pair<cxxime::IPCStatus, cxxime::ImeStatus> sync_caps_lock(uint32_t id, bool caps_lock);

    ProcessKeyResult process_key(uint32_t id, const cxxime::KeyEvent& event,
                                 uint32_t visible_candidate_count = 0);
    cxxime::CandidatePage search_candidates(const std::string& input);
    bool record_search_result(const std::string& input, const std::string& result);
    ProcessKeyResult select_candidate(uint32_t id, int index,
                                      uint64_t candidate_revision = 0);
    ProcessKeyResult commit_composition(uint32_t id);
    ProcessKeyResult clear_composition(uint32_t id);
    ProcessKeyResult focus_in(uint32_t id);
    // The program and window title the client's input goes to (input collection only).
    bool set_input_target(uint32_t id, const std::string& app, const std::string& window_title);
    // SET_CONTEXT: the text before the caret (flags: cxxime::kContextTextRead / kContextNewInputBox).
    bool set_context(uint32_t id, const std::string& text, uint32_t flags);
    ProcessKeyResult focus_out(uint32_t id);
    // Voice input: while capturing, the session's keys do not reach the input box
    // (Engine::set_voice_capture). False when there is no such session.
    bool set_voice_capture(uint32_t id, bool capturing);
    // What the session's client can do (kClientCapability*); 0 when there is no such session.
    uint64_t client_capabilities(uint32_t id);

    // Long pinyin with the continuation model in the background (laya_rerank.h): after a key of
    // a client that can fetch a page without one (kClientCapabilityCandidateRefresh), the paths
    // the model still has to score are computed on a worker; when they are in, the session's
    // composition is the same (Engine::continuation_key) and `notifier` can reach its client,
    // the client is told, and fetches the page (REFRESH_CANDIDATES). Off until started (tests,
    // tools).
    void start_background_continuation(RefreshNotifier notifier);
    void stop_background_continuation();
    // REFRESH_CANDIDATES: the session's composition and candidates, no key; the first page
    // composed again with the model's sentence first when its scores are in for this very
    // composition. Only here does the page change: a key the client sent before (a space for
    // the page it shows) is handled first, on that page.
    ProcessKeyResult refresh_candidates(uint32_t id);

    cxxime::IPCStatus add_user_entry(cxxime::UserDictKind kind, const std::string& text,
                                     const std::string& code,
                                     const std::string& syllables = {});
    cxxime::UserDictQueryResult query_user_entries(const std::string& query,
                                                   cxxime::UserDictKind kind,
                                                   size_t offset, size_t limit);
    cxxime::UserDictQueryResult query_lexicon_entries(
        cxxime::LexiconResource resource, const std::string& query,
        cxxime::UserDictKind kind, size_t offset, size_t limit, bool exact_text = false);
    cxxime::UserDictQueryResult query_disabled_system_entry_status(
        cxxime::UserDictKind kind, const std::vector<std::string>& texts);
    cxxime::IPCStatus delete_user_entries(
        cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries);
    cxxime::IPCStatus replace_user_entry(cxxime::UserDictKind kind,
                                         const std::string& old_text, const std::string& old_code,
                                         const std::string& new_text, const std::string& new_code,
                                         const std::string& syllables = {});
    cxxime::IPCStatus import_user_dict(cxxime::UserDictKind kind,
                                       const std::string& source_path);
    cxxime::IPCStatus save_user_dict(cxxime::UserDictKind kind);
    cxxime::IPCStatus disable_system_entry(cxxime::UserDictKind kind,
                                           const std::string& text);
    cxxime::IPCStatus restore_system_entry(cxxime::UserDictKind kind,
                                           const std::string& text);
    cxxime::IPCStatus delete_candidate_preferences(
        cxxime::UserDictKind kind, const std::vector<cxxime::LexiconEntryKey>& entries);
    cxxime::IPCStatus clear_candidate_preferences(cxxime::UserDictKind kind);
    cxxime::IPCStatus save_candidate_preferences(cxxime::UserDictKind kind);
    cxxime::CandidateOrderQueryResult query_candidate_order(cxxime::UserDictKind kind,
                                                            const std::string& code,
                                                            std::size_t limit);
    cxxime::IPCStatus replace_candidate_order(
        cxxime::UserDictKind kind, const std::string& code,
        const std::vector<cxxime::ManualCandidateOrderEntry>& entries,
        std::uint64_t expected_version, bool* version_conflict);
    cxxime::IPCStatus clear_candidate_order(cxxime::UserDictKind kind,
                                            const std::string& code,
                                            std::uint64_t expected_version,
                                            bool* version_conflict);
    bool save_candidate_preferences(bool force);
    bool snapshot_user_data(const std::vector<std::string>& file_names,
                            std::map<std::string, std::string>* files);
    // Whether every file holds valid contents for its name (nothing is changed).
    bool validate_user_data(const std::map<std::string, std::string>& files) const;
    void merge_user_data(const std::map<std::string, std::string>& files,
                         std::size_t* imported_count, std::size_t* skipped_count);
    bool freeze_and_save_candidate_preferences();
    bool close_english_learning();
    // The dictionary the sessions use now (word lists are applied to it).
    std::shared_ptr<cxxime::Dict> dictionary(cxxime::UserDictKind kind) const {
        return shared_.dict_for_kind(kind);
    }

private:
    struct GlobalVisibleState {
        bool caps_lock = false;
        cxxime::InputMode input_mode = cxxime::InputMode::PINYIN;
        bool english_words = true;  // English mode style (config english.word_mode)
        // Pinyin style (config engine.input_style with input_mode); kept while in Wubi mode.
        cxxime::PinyinStyle pinyin_style = cxxime::PinyinStyle::kFull;
    };

    cxxime::Engine* get_engine(uint32_t id);

    // Helper: two-phase lock lookup. Returns nullptr if session not found.
    std::shared_ptr<SessionEntry> lookup_session(uint32_t id);

    void reset_global_state(const SharedResourceSnapshot& resources);
    GlobalVisibleState snapshot_global_state();
    void commit_global_state(GlobalVisibleState next);
    void align_session_to_global(SessionEntry& entry);

    // engine.input_style from the shared input mode and pinyin style (the older
    // engine.input_mode and engine.pinyin_initials are removed).
    void persist_input_style();
    void persist_english_word_mode(bool enabled);
    // The style key in Chinese mode: full pinyin -> initials -> Wubi -> shuangpin -> full pinyin,
    // skipping Wubi without its dictionary and shuangpin without its spelling table.
    static void cycle_input_style(const cxxime::EngineRuntimeState* runtime,
                                  GlobalVisibleState& state);

    // After a key or a pick: the composition's continuation work to the worker, if any.
    void schedule_continuation(uint32_t id, SessionEntry& entry);
    void continuation_done(uint32_t id, const std::string& key);

    SharedResources shared_;
    std::unordered_map<uint32_t, std::shared_ptr<SessionEntry>> sessions_;
    GlobalVisibleState global_state_;
    uint32_t next_id_ = 1;
    std::mutex mutex_;
    std::mutex state_mutex_;
    std::mutex reload_mutex_;
    ConfigPatchHandler config_patch_handler_;
    std::mutex continuation_mutex_;
    std::unique_ptr<ContinuationWorker> continuation_worker_;
    RefreshNotifier refresh_notifier_;
};

#endif // CXXIME_SESSION_MANAGER_H_
