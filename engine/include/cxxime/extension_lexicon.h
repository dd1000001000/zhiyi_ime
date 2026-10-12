// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The words of the user's enabled word lists (word_list.h), queried beside the system
// dictionary by Dict: the same lookups (syllable ids for whole words and sentence composition,
// typed keys for the short-input fast path, Wubi code prefixes), the same scores, so an imported
// word ranks as a system word of its frequency would. Built in memory, read-only afterwards;
// Dict swaps in a new one when the lists change.
#ifndef CXXIME_EXTENSION_LEXICON_H_
#define CXXIME_EXTENSION_LEXICON_H_

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/word_list.h>

namespace cxxime {

class ExtensionLexicon {
public:
    struct Word {
        std::string text;
        std::string code;     // pinyin: syllables "a:li"; Wubi: the code
        std::string compact;  // pinyin: the syllables without separators; Wubi: empty
        int frequency = 0;
        std::uint32_t ids = 0;       // pinyin: first syllable id in ids_
        std::uint32_t id_count = 0;  // pinyin: number of syllables
        bool erhua = false;          // pinyin: ends in 儿 read er (typed r too)
        std::vector<std::string> syllables;  // pinyin: the syllables
    };

    // `syllabary`: the system dictionary's syllables, by id. Words with a syllable it lacks
    // are left out. `withheld`: words of disabled lists (see is_withheld).
    static std::shared_ptr<const ExtensionLexicon> build_pinyin(
        std::vector<ExtensionWord> words, const std::vector<std::string>& syllabary,
        std::vector<std::string> withheld = {});
    static std::shared_ptr<const ExtensionLexicon> build_wubi(std::vector<ExtensionWord> words,
                                                              std::vector<std::string> withheld = {});

    std::size_t size() const { return words_.size(); }
    bool empty() const { return words_.empty(); }

    // ── Pinyin, by the system dictionary's syllable ids ──
    bool has_prefix(const std::uint32_t* ids, std::size_t count) const;
    // The words whose syllables are exactly `ids` (most frequent first), then, unless
    // `exact_only`, those that start with them; at most `max_words` in all.
    std::vector<const Word*> match_ids(const std::uint32_t* ids, std::size_t count,
                                       bool exact_only, std::size_t max_words) const;
    // The words of these exact syllables ("a:li").
    std::vector<const Word*> match_syllables(std::string_view syllables) const;
    // The short-input fast path (as the Top-N index, scripts/build_pinyin_topn.py): the words
    // `key` spells in full, by initials, mixed or as the start of the word, scored as the index
    // scores them (frequency = score, source_frequency = the word's frequency). Words scoring
    // below `min_score` are left out (the caller has `limit` better ones already).
    std::vector<Candidate> lookup_key(const std::string& key, int limit, int min_score = INT_MIN) const;

    // ── Wubi ──
    // Words whose code starts with `prefix`: exact codes first, then shorter codes, then the
    // more frequent; at most `limit`.
    std::vector<const Word*> match_code_prefix(std::string_view prefix, std::size_t limit) const;

    // A word with this text and code (pinyin: syllables or the compact code; Wubi: the code).
    bool contains(std::string_view text, std::string_view code) const;
    // A word of a disabled list that no enabled list has: what was learned of it stays hidden.
    bool is_withheld(const std::string& text) const { return withheld_.count(text) != 0; }

private:
    ExtensionLexicon() = default;
    void build_key_index();
    bool reads_as_syllables(const std::string& key) const;
    // Whether a key reads as whole syllables: worked out once, and only when a score needs it.
    struct KeyIsSyllables {
        const ExtensionLexicon* lexicon;
        int value = -1;
        bool operator()(const std::string& key) {
            if (value < 0) value = lexicon->reads_as_syllables(key) ? 1 : 0;
            return value == 1;
        }
    };
    // The Top-N index's score of `word` for `key`; false when the key does not spell it.
    bool key_score(const std::string& key, const Word& word, KeyIsSyllables& key_is_syllables,
                   int* score) const;
    Candidate key_candidate(const Word& word, int score) const;

    bool pinyin_ = true;
    std::vector<Word> words_;  // pinyin: by syllable ids, then frequency; Wubi: by code
    std::vector<std::uint32_t> ids_;
    std::vector<std::uint32_t> by_code_;     // pinyin: indexes by code (syllables)
    std::vector<std::uint32_t> by_compact_;  // pinyin: indexes by compact code
    std::unordered_set<std::string> syllables_;  // pinyin: the system syllables
    // Fast-path keys: each word's complete keys (exact, initials, mixed), sorted.
    struct Key {
        std::uint32_t offset;
        std::uint32_t length;
        std::uint32_t word;
    };
    std::string key_chars_;
    std::vector<Key> keys_;
    // Keys of up to kShortKeyLength letters, which start thousands of words: their best words,
    // scored when built (word index, score), best first.
    struct Scored {
        std::uint32_t word;
        int score;
    };
    std::unordered_map<std::string, std::vector<Scored>> short_keys_;
    std::unordered_set<std::string> withheld_;
};

}  // namespace cxxime

#endif  // CXXIME_EXTENSION_LEXICON_H_
