// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// English word list for word candidates and completion (data/english.words.tsv, built by
// data/tools/build_english_dictionary.py from the rime-ice English dictionaries).
#ifndef CXXIME_ENGLISH_LEXICON_H_
#define CXXIME_ENGLISH_LEXICON_H_

#include <memory>
#include <string>
#include <vector>

namespace cxxime {

struct EnglishWord {
    std::string text;   // case preserved, e.g. "README.md", "Los Angeles"
    int score = 0;      // 100 * Zipf frequency
    bool exact = false; // the typed code equals the word's code
    // Spelling correction: weighted edit cost between the typed code and the word (0 for
    // dictionary prefix matches). Learned corrections lower it (negative = bonus).
    float cost = 0.0f;
    bool corrected = false;
};

// Weighted edit cost of typing `typed` for `word` (both lowercase letters): adjacent QWERTY
// keys and doubled letters are cheap, a wrong first letter is expensive. `prefix` compares
// against the cheapest prefix of `word` (a word still being typed). Exposed for tests.
float english_typing_cost(const std::string& typed, const std::string& word, bool prefix = false);

// Corrections are ranked by Zipf frequency minus this many Zipf units per unit of typing cost.
constexpr float kEnglishCostRankWeight = 3.0f;

// Largest correction cost allowed for a typed code of `length` letters (0 = no correction).
float english_correction_limit(std::size_t length);

class EnglishLexicon {
public:
    // Process-wide instance loaded once from data_path("english.words.tsv").
    // Returns nullptr when the file is missing or empty.
    static std::shared_ptr<const EnglishLexicon> shared();

    bool load(const std::string& path);
    std::size_t size() const { return entries_.size(); }

    // Words whose code starts with `code` (case-insensitive): exact matches first, then
    // completions by descending score. Completions below `min_score` are skipped; exact
    // matches are kept when `include_rare_exact` is true.
    std::vector<EnglishWord> lookup(const std::string& code, int limit, int min_score,
                                    bool include_rare_exact = true) const;

    // True when `code` (case-insensitive) is a word of the list / the start of one.
    bool contains(const std::string& code) const;
    bool has_prefix(const std::string& code) const;

    // Spelling corrections for a misspelled code: plain words (no names or phrases) scoring at
    // least `min_score` whose spelling is within english_correction_limit() of the code, best
    // first (frequency minus a cost penalty). With `completions`, words whose beginning is
    // within the limit count too (beautf -> beautiful); otherwise only whole words.
    std::vector<EnglishWord> corrections(const std::string& code, int limit, int min_score,
                                         bool completions) const;

private:
    struct Entry {
        std::string key;   // lowercase code
        std::string text;
        int score = 0;
    };
    std::vector<Entry> entries_;  // sorted by key
};

}  // namespace cxxime

#endif  // CXXIME_ENGLISH_LEXICON_H_
