// Copyright (c) 2026 Laya IME Contributors. Apache License 2.0.
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
};

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
