// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// English word mode candidates (before Laya reranking): dictionary words starting with the
// typed letters, spelling corrections and learned words.
#ifndef CXXIME_ENGLISH_CANDIDATES_H_
#define CXXIME_ENGLISH_CANDIDATES_H_

#include <string>
#include <vector>

#include <cxxime/config.h>
#include <cxxime/english_lexicon.h>

namespace cxxime {

class EnglishLearning;

struct EnglishPool {
    // Exact match first, then completions, user words and corrections by frequency (a
    // correction's frequency is lowered by its typing cost). Texts are in the typed case.
    std::vector<EnglishWord> words;
    // A correction the user picked EnglishLearning::kActiveCount times or more: it takes the
    // recommended slot whatever the model prefers. Empty when there is none.
    std::string learned_pick;
};

// `learning` is nullptr when self-learning is off.
EnglishPool build_english_pool(const EnglishLexicon& lexicon, const EnglishLearning* learning,
                               const std::string& typed, const Config::EnglishConfig& config);

// True when `typed` is a common word as it is (a dictionary word scoring min_score or more).
bool english_common_word(const EnglishLexicon& lexicon, const std::string& typed,
                         const Config::EnglishConfig& config);

// "HEL" -> "HELLO", "Hel" -> "Hello", "hel" -> the dictionary form ("hello", "iPhone").
std::string match_typed_case(const std::string& typed, const std::string& word);

}  // namespace cxxime

#endif  // CXXIME_ENGLISH_CANDIDATES_H_
