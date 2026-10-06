// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PINYIN_COMPOSER_H_
#define CXXIME_PINYIN_COMPOSER_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cxxime/candidate.h>

namespace cxxime {

class Dict;
struct QueryDeadline;

enum class CompositionPathKind : uint8_t {
    kNormal,
    kRepeatedShortCode,
};

struct CompositionPath {
    const std::vector<uint32_t>* ids = nullptr;
    const std::vector<std::string>* syllables = nullptr;
    const std::string* input_code = nullptr;
    CompositionPathKind kind = CompositionPathKind::kNormal;
    uint16_t rank = 0;
};

struct CompositionLimits {
    uint32_t max_normal_paths = 8;
    uint32_t max_repeated_short_paths = 1;
    uint32_t max_range_queries = 128;
    uint32_t max_entry_scans = 2048;
    // Homophones per syllable span (Rime's max_homophones): the model picks among the sentences.
    uint32_t max_candidates_per_range = 3;
    uint32_t max_span_candidates = 256;
    uint32_t max_beam_width = 32;
    uint32_t max_nodes = 1024;
    uint32_t max_final_candidates = 32;
    // Rime's MakeSentences: at most this many sentences, each within this relative distance of
    // the previous one's log-probability (the distance tightens as sentences are added).
    uint32_t max_sentences = 5;
    double sentence_cutoff = 0.1;
    // The best sentence of another syllable path (xian'e / xia'ne) within this distance of the best.
    double path_cutoff = 0.45;
};

struct CompositionStats {
    uint32_t normal_path_count = 0;
    uint32_t repeated_short_path_count = 0;
    uint32_t span_query_count = 0;
    uint32_t span_entry_scan_count = 0;
    uint32_t span_candidate_count = 0;
    uint32_t state_count = 0;
    uint32_t candidate_count = 0;
    bool truncated = false;
    bool deadline_exceeded = false;
};

// A word's weight in a sentence, as Rime's Poet without a grammar model: ln(frequency / 1e8)
// plus ln(1e-6) for every word, so fewer and more common words win. In thousandths.
constexpr double kComposedScoreScale = 1000.0;
constexpr double kComposedLogTotalWeight = 18.420680743952367;  // ln(1e8)
constexpr double kComposedWordPenalty = -13.815510557964274;    // ln(1e-6)

inline int64_t composed_word_score(int frequency) {
    const double weight = std::log(static_cast<double>((std::max)(0, frequency)) + 1.0) -
                          kComposedLogTotalWeight + kComposedWordPenalty;
    return static_cast<int64_t>(std::llround(weight * kComposedScoreScale));
}

// Where a sentence ranks among the dictionary words. It covers the whole input, so it comes
// before the words that only extend it (kExtensionBase, 30,000,000). Against a completion
// (45,000,000: a word whose unfinished last syllable the typist is still on, 晚上 for "wansha")
// Rime compares weights, and a common word's ln(frequency) + ln(0.05) is far above any sentence
// of two or more words with their ln(1e-6) each: sentences take the bottom of the completion
// group, below every completion (whose scores start around 5,000,000 above the base) and above
// every extension. Within that band the aggregate score (a sum of composed_word_score) decides.
constexpr int kSentenceBase = 45000000;
constexpr int64_t kSentenceScoreSpan = 4999999;

// When a dictionary word already covers the whole input (complete or by completion), the
// sentences are alternatives of last resort and go below every word, as Rime drops its sentence
// when a phrase spans the input: the extensions (抹黑中国 for "mohei") keep their places.
constexpr int kCoveredSentenceBase = 1;

inline int covered_sentence_frequency(int frequency) {
    return kCoveredSentenceBase + (std::max)(0, frequency - kSentenceBase) / 5;
}

inline int sentence_frequency(int64_t aggregate_score) {
    // Aggregates run from about -30,000 (two common words) to below -200,000: 20 per unit
    // keeps that range inside the band above kSentenceBase.
    const int64_t scaled = (std::max)(int64_t{0}, (std::min)(kSentenceScoreSpan,
                                                            kSentenceScoreSpan + aggregate_score * 20));
    return kSentenceBase + static_cast<int>(scaled);
}

class PinyinComposer {
public:
    explicit PinyinComposer(const Dict& dict)
        : dict_(dict) {}

    std::vector<Candidate> compose(const std::string& input,
                                   const std::vector<CompositionPath>& paths,
                                   size_t requested_candidates, const QueryDeadline& deadline,
                                   const CompositionLimits& limits, CompositionStats& stats) const;

private:
    const Dict& dict_;
};

} // namespace cxxime

#endif // CXXIME_PINYIN_COMPOSER_H_
