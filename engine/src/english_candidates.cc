// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/english_candidates.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <cxxime/english_learning.h>

namespace cxxime {

namespace {

// A learned word ranks like a common dictionary word (Zipf 4.5), more so when used often.
constexpr int kUserWordScore = 450;
constexpr int kUserWordScorePerUse = 10;
// Typing cost taken off a correction picked once (EnglishLearning::kActiveCount picks make it
// the recommended word instead).
constexpr float kLearnedCorrectionBonus = 0.5f;

std::string lowercase(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

float rank(const EnglishWord& w) { return w.score / 100.0f - kEnglishCostRankWeight * w.cost; }

}  // namespace

std::string match_typed_case(const std::string& typed, const std::string& word) {
    int letters = 0, upper = 0;
    for (char c : typed) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            ++letters;
            upper += std::isupper(static_cast<unsigned char>(c)) ? 1 : 0;
        }
    }
    std::string out = word;
    if (letters >= 2 && upper == letters) {
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    } else if (!typed.empty() && std::isupper(static_cast<unsigned char>(typed[0])) && !out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

bool english_common_word(const EnglishLexicon& lexicon, const std::string& typed,
                         const Config::EnglishConfig& config) {
    const auto words = lexicon.lookup(typed, 1, config.min_score, false);
    return !words.empty() && words.front().exact;
}

EnglishPool build_english_pool(const EnglishLexicon& lexicon, const EnglishLearning* learning,
                               const std::string& typed, const Config::EnglishConfig& config) {
    EnglishPool pool;
    const int size = (std::max)({0, config.completion_count, config.completion_pool});
    const std::string key = lowercase(typed);

    // Dictionary order: exact match first, then completions by frequency.
    std::vector<EnglishWord> exact, others;
    for (auto& w : lexicon.lookup(typed, size, config.min_score, true)) {
        w.text = match_typed_case(typed, w.text);
        (w.exact ? exact : others).push_back(std::move(w));
    }
    if (learning) {
        for (const auto& u : learning->user_words_with_prefix(typed, size)) {
            EnglishWord w{u.text, kUserWordScore + kUserWordScorePerUse * (std::min)(u.count, 10),
                          lowercase(u.text) == key};
            (w.exact ? exact : others).push_back(std::move(w));
        }
    }

    // Spelling corrections, unless what was typed is a common word or (the start of) a word
    // the user keeps typing as it is.
    const int max_corrections = (std::max)(0, config.correction_count);
    const bool correct = config.correction && max_corrections > 0 &&
                         !english_common_word(lexicon, typed, config) &&
                         !(learning && learning->is_user_word_prefix(typed));
    if (correct) {
        // Completing a misspelled beginning only once there is enough of it, and only when no
        // dictionary word starts with it.
        const bool completions = key.size() >= 5 && !lexicon.has_prefix(key);
        std::vector<EnglishWord> corrections =
            lexicon.corrections(typed, max_corrections + 2, config.min_score, completions);
        if (learning) {
            for (const auto& learned : learning->corrections_for(typed)) {
                auto it = std::find_if(corrections.begin(), corrections.end(),
                                       [&](const EnglishWord& w) { return w.text == learned.word; });
                if (it == corrections.end()) {
                    // Picked before but not among the best corrections now: add it back.
                    for (auto& w : lexicon.lookup(learned.word, 4, 0, true)) {
                        if (w.exact && w.text == learned.word) {
                            w.exact = false;
                            w.corrected = true;
                            w.cost = english_typing_cost(key, lowercase(w.text), completions);
                            corrections.push_back(std::move(w));
                            it = corrections.end() - 1;
                            break;
                        }
                    }
                    if (it == corrections.end()) continue;  // no longer in the dictionary
                }
                it->cost -= kLearnedCorrectionBonus;
                if (learned.count >= EnglishLearning::kActiveCount && pool.learned_pick.empty())
                    pool.learned_pick = match_typed_case(typed, it->text);
            }
        }
        std::stable_sort(corrections.begin(), corrections.end(),
                         [](const EnglishWord& a, const EnglishWord& b) { return rank(a) > rank(b); });
        if (static_cast<int>(corrections.size()) > max_corrections) {
            // Keep the learned pick even when it ranks lower.
            auto pick = std::find_if(corrections.begin(), corrections.end(), [&](const EnglishWord& w) {
                return match_typed_case(typed, w.text) == pool.learned_pick;
            });
            if (pick != corrections.end() && pick - corrections.begin() >= max_corrections)
                std::rotate(corrections.begin(), pick, pick + 1);
            corrections.resize(static_cast<size_t>(max_corrections));
        }
        for (auto& w : corrections) {
            w.text = match_typed_case(typed, w.text);
            others.push_back(std::move(w));
        }
        if (!corrections.empty()) {
            // A rare word spelled as typed is usually a listed misspelling (definately): it must
            // not take the recommended slot from the correction. The typed text stays first.
            exact.erase(std::remove_if(exact.begin(), exact.end(),
                                       [&](const EnglishWord& w) {
                                           return w.score < config.min_score &&
                                                  lowercase(w.text) == key;
                                       }),
                        exact.end());
        }
    }

    std::stable_sort(others.begin(), others.end(),
                     [](const EnglishWord& a, const EnglishWord& b) { return rank(a) > rank(b); });
    std::unordered_set<std::string> seen;
    for (auto* list : {&exact, &others}) {
        for (auto& w : *list) {
            if (seen.insert(w.text).second) pool.words.push_back(std::move(w));
        }
    }
    return pool;
}

}  // namespace cxxime
