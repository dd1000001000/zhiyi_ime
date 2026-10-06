// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_DICT_H_
#define CXXIME_DICT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/short_code_cache.h>
#include <cxxime/user_dict.h>
#include <cxxime/user_data_merge.h>

namespace cxxime {

struct QueryTrace;
struct QueryBudget;
struct QueryDeadline;
struct UserLookupStats;
class CandidatePreference;
class DisabledSystemLexicon;
class ManualCandidateOrder;
class UserLexicon;
class WubiPrefixIndex;

struct SpanCandidate {
    uint16_t end = 0;
    Candidate candidate;
};

struct SpanLookupLimits {
    uint32_t max_range_queries = 128;
    uint32_t max_entry_scans = 2048;
    uint32_t max_results = 256;
    uint32_t max_candidates_per_range = 8;
};

struct SpanLookupStats {
    uint32_t range_queries = 0;
    uint32_t entry_scans = 0;
    uint32_t result_count = 0;
    bool truncated = false;
    bool deadline_exceeded = false;
};

struct UserLookupStats {
    uint32_t scan_count = 0;
    bool truncated = false;
    bool scan_budget_truncated = false;
    bool deadline_exceeded = false;
};

class Dict {
public:
    explicit Dict(UserDictKind kind);
    ~Dict();
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;

    // Convenience: load main dict (.bin) + open user dict (TSV file)
    bool open(const std::string& dict_path, const std::string& user_dict_path = "");
    bool open_bundle(const std::string& dict_path,
                     const std::string& user_dict_path,
                     const std::string& idx_path,
                     const std::string& topn_path);
    bool open_wubi_dict(const std::string& dict_path,
                        const std::string& prefix_index_path);
    bool open_wubi_bundle(const std::string& dict_path,
                          const std::string& user_dict_path,
                          const std::string& prefix_index_path);
    void close();   // saves user dict before closing
    bool is_open() const;
    UserDictKind kind() const { return kind_; }

    // Low-level
    bool open_dict(const std::string& bin_path);
    void unload_dict();

    // Queries (non-budget versions — for tools/tests, not hot path)
    std::vector<Candidate> lookup(const std::string& code_prefix, int limit = 10, QueryTrace* trace = nullptr);
    std::vector<Candidate> lookup_by_syllables(const std::vector<std::string>& syllables, int limit = 10, QueryTrace* trace = nullptr);
    // Budget-aware versions (hot path — with deadline and scan limits)
    std::vector<Candidate> lookup(const std::string& code_prefix, int limit, const QueryBudget& budget, QueryTrace* trace = nullptr);
    std::vector<Candidate> lookup_by_syllables(const std::vector<std::string>& syllables, int limit, const QueryBudget& budget, QueryTrace* trace = nullptr);
    std::vector<Candidate> lookup_by_ids(const std::vector<uint32_t>& ids, int limit = 10,
                                         QueryTrace* trace = nullptr, const QueryBudget* budget = nullptr);
    bool lookup_exact_span(const std::vector<uint32_t>& ids,
                           size_t start,
                           size_t end,
                           const SpanLookupLimits& limits,
                           const QueryDeadline& deadline,
                           std::vector<Candidate>& output,
                           SpanLookupStats& stats) const;
    void lookup_exact_spans(const std::vector<uint32_t>& ids,
                            size_t start,
                            const SpanLookupLimits& limits,
                            const QueryDeadline& deadline,
                            std::vector<SpanCandidate>& output,
                            SpanLookupStats& stats) const;
    bool has_prefix(const std::vector<uint32_t>& ids, QueryTrace* trace = nullptr) const;
    int count(const std::string& code_prefix, QueryTrace* trace = nullptr);
    std::string reverse_lookup(const std::string& text);
    bool add_user_entry(const std::string& text, const std::string& code,
                        const std::string& syllables = {});
    std::vector<UserDictEntryInfo> query_user_entries(const std::string& query,
                                                      size_t offset,
                                                      size_t limit,
                                                      size_t* match_total = nullptr,
                                                      bool exact_text = false) const;
    bool delete_user_entries(const std::vector<LexiconEntryKey>& entries);
    bool replace_user_entry(const std::string& old_text, const std::string& old_code,
                            const std::string& new_text, const std::string& new_code,
                            const std::string& syllables = {});
    bool add_user_entry_and_save(const std::string& text, const std::string& code,
                                 const std::string& syllables = {});
    bool delete_user_entries_and_save(const std::vector<LexiconEntryKey>& entries);
    bool replace_user_entry_and_save(const std::string& old_text, const std::string& old_code,
                                     const std::string& new_text, const std::string& new_code,
                                     const std::string& syllables = {});
    bool import_user_dict(const std::string& source_path);
    bool merge_user_dict_contents(const std::string& imported, UserDataMergeResult* result);

    // User dictionary persistence
    bool load_user_dict(const std::string& path);
    bool save_user_dict();
    bool load_candidate_preferences(const std::string& path);
    bool save_candidate_preferences();
    bool merge_candidate_preference_contents(const std::string& imported,
                                             UserDataMergeResult* result);
    bool save_candidate_preferences_if_due(std::chrono::milliseconds delay);
    void freeze_candidate_preferences();
    bool record_candidate_preference(const Candidate& candidate, const std::string& code);
    void apply_candidate_preferences(const std::string& code, CandidateSource source,
                                     std::vector<Candidate>& candidates, int limit) const;
    std::vector<UserDictEntryInfo> query_candidate_preferences(
        const std::string& query, size_t offset, size_t limit,
        size_t* match_total = nullptr) const;
    bool delete_candidate_preferences(const std::vector<LexiconEntryKey>& entries);
    bool clear_candidate_preferences();
    bool delete_candidate_preferences_and_save(const std::vector<LexiconEntryKey>& entries);
    bool clear_candidate_preferences_and_save();
    bool clear_candidate_preferences_for_code_and_save(const std::string& code);
    size_t candidate_preference_count() const;
    uint64_t candidate_preference_version() const;
    bool load_manual_candidate_order(const std::string& path);
    bool merge_manual_candidate_order_contents(const std::string& imported,
                                               UserDataMergeResult* result);
    void apply_manual_candidate_order(const std::string& code, CandidateSource source,
                                      std::vector<Candidate>& candidates, int limit,
                                      std::string_view candidate_input_code = {}) const;
    std::vector<ManualCandidateOrderEntry> manual_candidate_order(
        const std::string& code) const;
    bool replace_manual_candidate_order_and_save(
        const std::string& code, const std::vector<ManualCandidateOrderEntry>& entries);
    bool replace_manual_candidate_order_if_version(
        const std::string& code, const std::vector<ManualCandidateOrderEntry>& entries,
        uint64_t expected_version, bool* version_conflict);
    bool has_manual_candidate_order(const std::string& input_code,
                                    const std::string& text,
                                    const std::string& candidate_code,
                                    const std::string& syllables) const;
    bool has_candidate_preference(const std::string& text, const std::string& code) const;
    bool can_resolve_manual_candidate(const ManualCandidateOrderEntry& entry,
                                      CandidateSource source) const;
    uint64_t manual_candidate_order_version() const;
    bool load_disabled_system_entries(const std::string& path);
    bool merge_disabled_system_contents(const std::string& imported,
                                        UserDataMergeResult* result);
    bool save_disabled_system_entries();
    bool disable_system_entry(const std::string& text);
    bool restore_system_entry(const std::string& text);
    bool disable_system_entry_and_save(const std::string& text);
    bool restore_system_entry_and_save(const std::string& text);
    bool is_system_entry_disabled(const std::string& text) const;
    void filter_disabled_system_candidates(std::vector<Candidate>& candidates) const;
    std::vector<UserDictEntryInfo> query_disabled_system_entries(
        const std::string& query, size_t offset, size_t limit,
        size_t* match_total = nullptr) const;
    size_t disabled_system_entry_count() const;
    uint64_t disabled_system_entry_version() const;

    // Short-code cache fast path
    const ShortCodeCache& short_cache() const { return short_cache_; }
    bool has_short_cache() const { return short_cache_.is_loaded(); }
    CandidateStoreView candidate_store() const;
    bool has_wubi_prefix_index() const;

    // User dictionary version for cache invalidation
    uint64_t user_dict_version() const;
    bool has_user_entry(const std::string& text) const;
    size_t user_entry_count() const;

    // Internal indexed user dictionary query methods
    std::vector<Candidate> lookup_user_exact(const std::string& code, int limit,
        const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;
    std::vector<Candidate> lookup_user_prefix(const std::string& prefix, int limit,
        const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;
    std::vector<Candidate> lookup_user_indexed(const std::string& key, int limit,
        const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;

    // Syllable ID mapping (for pinyin integer-ID lookup path)
    uint32_t syllable_to_id(const std::string& syllable) const;
    bool has_syllabary() const { return !syllable_to_id_.empty(); }

    // Test helper: create a binary dict file from entries
    static bool create_test_dict(const std::string& path,
                                 const std::vector<std::tuple<std::string, std::string, int>>& entries);

private:
    bool load_id_index(const std::string& dict_bin_path);
    bool load_id_index_file(const std::string& idx_path);
    bool open_dict_with_aux(const std::string& bin_path,
                            const std::string& idx_path,
                            const std::string& topn_path,
                            bool derive_aux_paths,
                            bool use_wubi_prefix_index = false);
    void build_syllabary();
    void build_id_index();
    void unload_id_index();
    void fill_system_candidate(uint32_t entry_index, Candidate& candidate,
                               int frequency_boost) const;
    bool preference_candidate_available(const Candidate& candidate,
                                        CandidateSource source) const;
    bool resolve_manual_candidate(const ManualCandidateOrderEntry& entry,
                                  CandidateSource source, Candidate* candidate) const;

    const UserDictKind kind_;
    char* dict_data_ = nullptr;         // heap-allocated buffer
    size_t dict_data_size_ = 0;
    const CandidateStoreEntry* dict_entries_ = nullptr;
    const char* dict_strings_ = nullptr;
    uint32_t dict_entry_count_ = 0;
    uint32_t dict_string_size_ = 0;
    uint64_t dict_fingerprint_ = 0;

    // Short-code cache
    ShortCodeCache short_cache_;
    std::unique_ptr<WubiPrefixIndex> wubi_prefix_index_;

    // Integer ID index (.dict.idx, heap-allocated)
    char* idx_data_ = nullptr;
    size_t idx_data_size_ = 0;

    // Integer ID index (librime-style syllable ID lookup)
    std::vector<std::string> syllabary_;
    std::unordered_map<std::string, uint32_t> syllable_to_id_;
    struct IdEntry { const uint32_t* ids; uint32_t count; uint32_t index; };
    std::vector<IdEntry> id_index_;
    std::vector<std::vector<uint32_t>> runtime_ids_;  // backing for build_id_index

    std::unique_ptr<UserLexicon> user_lexicon_;
    std::unique_ptr<CandidatePreference> candidate_preference_;
    std::unique_ptr<ManualCandidateOrder> manual_candidate_order_;
    std::unique_ptr<DisabledSystemLexicon> disabled_system_lexicon_;

};

} // namespace cxxime

#endif // CXXIME_DICT_H_
