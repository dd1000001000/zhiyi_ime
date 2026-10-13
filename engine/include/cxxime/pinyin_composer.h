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
    // ... and only those within 1 / homophone_ratio of the span's most common word (改 / 该, not
    // 瓦 / 哇); 0 = any.
    uint32_t homophone_ratio = 10;
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

// A model's word scores for the lattice (laya_rerank.h: the unified model's continuation head
// reads the typed syllables and gives ln P of every character at every syllable). A word
// spanning syllables [start, end) of a prepared path then weighs
//   ln P(its characters) + word_weight() * its composed_word_score (and joint penalties),
// in thousandths like every edge; the beam search is otherwise the same. Paths the scorer
// cannot prepare (a reading the typed code does not spell, more syllables than it reads, or
// past its budget of model calls) are left out of the composition, and words it cannot score
// are left out of the lattice.
class SpanScorer {
public:
    virtual ~SpanScorer() = default;
    // Called once per path before its spans; false = skip this path. `input` is the code typed.
    virtual bool prepare(const CompositionPath& path, const std::string& input) = 0;
    // ln P of `text` (one character per syllable) at syllables [start, end) of the prepared
    // path; false when the word cannot be scored.
    virtual bool score(const std::string& text, size_t start, size_t end, double& ln_p) const = 0;
    virtual double word_weight() const = 0;
    // Paths with fewer syllables are not the model's business (the translator keeps its limits).
    virtual size_t min_syllables() const { return 1; }
    // Whether the model scored anything (then the composed sentences are its).
    virtual bool scored() const = 0;
    // The translator composed without the model after all (nothing it scored made a sentence):
    // scored() is false from then on.
    virtual void discard() {}

    // A decoder model (laya_rerank.h): it reads the characters chosen so far, so a sentence is
    // scored a character at a time, in order (sequential_compose), not word by word; score() is
    // not used. step() gives, for each prefix (the characters chosen at the slots before `slot`,
    // one each), the distribution of the character at `slot` of the prepared path; false when
    // the model cannot (the search stops). char_ln_p reads it: ln P of `utf8_char` for
    // prefixes[prefix]; false when the model does not write that character.
    virtual bool sequential() const { return false; }
    virtual bool step(const std::vector<std::string>& prefixes, size_t slot) {
        (void)prefixes;
        (void)slot;
        return false;
    }
    virtual bool char_ln_p(size_t prefix, const std::string& utf8_char, double& ln_p) const {
        (void)prefix;
        (void)utf8_char;
        (void)ln_p;
        return false;
    }
    // Sequential models: the paths kept at every character.
    virtual size_t beam_width() const { return 16; }
};

// A dictionary word spanning the slots [start, end) of a path, for sequential_compose.
struct SequentialEdge {
    uint16_t end = 0;
    int64_t score = 0;                  // its composed_word_score (and joint penalties)
    const std::string* text = nullptr;  // one character per slot
};

struct SequentialPath {
    std::vector<std::pair<uint16_t, uint32_t>> edges;  // (start slot, index in edges_by_start[start])
    double ln_p = 0.0;                                  // the model's, summed over the characters
    int64_t word_score = 0;                             // the words' scores, summed
    int64_t score = 0;  // ln_p x kComposedScoreScale + word_weight() x word_score
};

// Sequential models: a beam search a character at a time over `slots` slots. At every slot the
// model is asked once for the paths kept (scorer.step); a path inside a word takes the word's
// next character, a path at a word boundary starts every word of edges_by_start[slot] (none at
// an ambiguous joint), and the best beam_width() paths by score go on. The model's time moves
// the deadline back (as the composer does for prepare). Returns the complete paths, best first,
// at most max_results; empty when the model fails, no word covers the slots, or the deadline
// passes (deadline_hit).
std::vector<SequentialPath> sequential_compose(size_t slots,
                                               const std::vector<std::vector<SequentialEdge>>& edges_by_start,
                                               const std::vector<uint8_t>* ambiguous_joint,
                                               SpanScorer& scorer, size_t max_results,
                                               QueryDeadline& deadline, bool& deadline_hit);

class PinyinComposer {
public:
    explicit PinyinComposer(const Dict& dict)
        : dict_(dict) {}

    std::vector<Candidate> compose(const std::string& input,
                                   const std::vector<CompositionPath>& paths,
                                   size_t requested_candidates, const QueryDeadline& deadline,
                                   const CompositionLimits& limits, CompositionStats& stats,
                                   SpanScorer* scorer = nullptr) const;

private:
    const Dict& dict_;
};

} // namespace cxxime

#endif // CXXIME_PINYIN_COMPOSER_H_
