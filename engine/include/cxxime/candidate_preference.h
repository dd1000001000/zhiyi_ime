// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Candidate preferences: what the user picked for a typed code, as Rime's user dictionary
// remembers commits. Every selection adds one commit and raises a decaying weight ("dee");
// the weight loses a factor e every 200 selections of any word (user_dictionary.cc in librime).
//   - one commit so far ("tentative"): the word is boosted within its ranking group
//     (kLearnedBoost, about x20 in frequency); it is forgotten when its weight decays below
//     kTentativeExpiry (about 210 selections of other words);
//   - two commits or more ("confirmed"): the word is pinned ahead of the dictionary words
//     while its weight stays above kPinFloor, and boosted only after a long time unused.
// Wubi has no frequency scale to boost within, so a single commit already pins there.
// Pinyin words are found for any way of typing them (pinyin_spelling_match.h): one entry per
// word and syllables, whatever letters were typed when it was picked, as Rime's user
// dictionary keys by syllables. A word matched through initials or an unfinished syllable
// carries that spelling's credibility into its score.
#ifndef CXXIME_CANDIDATE_PREFERENCE_H_
#define CXXIME_CANDIDATE_PREFERENCE_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/user_data_merge.h>
#include <cxxime/user_dict.h>

namespace cxxime {

// Score added to a tentative word within its ranking group: 500000 x ln(20), as x20 frequency.
constexpr int kLearnedBoost = 1500000;
// A tentative word that the dictionary query no longer lists: shown as a weak completion.
constexpr int kLearnedAbsentScore = 50000000;
// Frequencies in [30M, 75M) are the pinyin ranking groups of 15,000,000 each; a boost stays
// inside the group (candidate-selection.md).
constexpr int kRankingGroupSpan = 15000000;
constexpr int kRankingGroupsBegin = 30000000;
constexpr int kRankingGroupsEnd = 75000000;
// Returned by preferred_candidates() as the frequency of a tentative (boost only) word; its
// source_frequency then carries the ranking score the word had when it was picked (0 when
// unknown), so a word outside the fetched window can still be boosted into view.
constexpr int kLearnedBoostRequest = -1;
// The ranking's log scale (500000 x ln): a spelling credibility in that scale.
constexpr int kRankingLogScale = 500000;

// What one record() changed, so the commit it belongs to can be taken back (Backspace right
// after the commit: Rime's "forget about last commit").
struct CandidatePreferenceReceipt {
    std::string text;
    std::string code;
    bool created = false;
    int previous_frequency = 0;
    double previous_dee = 0.0;
    std::uint64_t previous_sequence = 0;
    std::uint64_t sequence = 0;  // the entry's sequence after the record
};

class CandidatePreference {
public:
    explicit CandidatePreference(UserDictKind kind) : kind_(kind) {}

    bool load(const std::string& path);
    bool save();
    static bool validate_contents(const std::string& contents);
    bool save_if_due(std::chrono::milliseconds delay);
    bool merge_contents_and_save(const std::string& imported, UserDataMergeResult* result);
    void freeze();

    bool record(const Candidate& candidate, const std::string& code,
                CandidatePreferenceReceipt* receipt = nullptr);
    // Takes back a record() when nothing else touched the entry since.
    bool revoke(const CandidatePreferenceReceipt& receipt);
    // The learned words for `code`: those recorded under exactly this code, and (pinyin) those
    // whose syllables the code spells (initials, unfinished last syllable). frequency = pinned
    // score, or kLearnedBoostRequest for a tentative word (the caller boosts it within the
    // dictionary results). Expired words are left out.
    std::vector<Candidate> preferred_candidates(const std::string& code,
                                                CandidateSource source) const;
    // Forgets the word recorded under (text, code), or the word (text, syllables) whatever code
    // it was recorded under. False when there is none.
    bool forget(const std::string& text, const std::string& code, const std::string& syllables);
    std::vector<UserDictEntryInfo> query(const std::string& query, std::size_t offset,
                                         std::size_t limit,
                                         std::size_t* match_total = nullptr) const;
    bool erase(const std::vector<LexiconEntryKey>& entries);
    bool clear();
    bool erase_and_save(const std::vector<LexiconEntryKey>& entries);
    bool erase_code_and_save(const std::string& code);
    bool clear_and_save();
    bool contains(const std::string& text, const std::string& code) const;
    std::size_t entry_count() const;
    std::uint64_t version() const;
    bool dirty() const;

private:
    using EntryId = std::uint32_t;
    static constexpr EntryId kNoEntry = static_cast<EntryId>(-1);

    struct Entry {
        std::string text;
        std::string code;
        std::string candidate_code;
        std::string syllables;
        int frequency = 1;           // commits
        std::uint64_t sequence = 0;  // tick of the last commit
        double dee = 1.0;            // decaying weight as of `sequence`
        int score = 0;               // the word's ranking score when last picked (0: unknown)
        bool deleted = false;
    };

    static std::string entry_key(const std::string& text, const std::string& code);
    static std::string serialize_entries(const std::vector<Entry>& entries,
                                         std::uint64_t current_sequence);
    static bool parse_entry(const std::vector<std::string>& fields, Entry* entry);
    void rebuild_indexes_locked();

    const UserDictKind kind_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, EntryId> entry_index_;
    std::unordered_map<std::string, std::vector<EntryId>> code_index_;
    // Entries with syllables, by the first letter of the syllables: the bucket a typed input
    // is matched against.
    std::unordered_map<char, std::vector<EntryId>> initial_index_;
    EntryId find_by_syllables_locked(const std::string& text, const std::string& syllables) const;
    std::atomic<std::uint64_t> version_{0};
    std::uint64_t sequence_ = 0;
    std::atomic<std::uint64_t> last_update_ms_{0};
    mutable std::shared_mutex mutex_;
    std::mutex save_mutex_;
    std::atomic<bool> dirty_{false};
    bool accepting_updates_ = true;
    std::string path_;
};

} // namespace cxxime

#endif // CXXIME_CANDIDATE_PREFERENCE_H_
