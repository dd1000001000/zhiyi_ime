// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PINYIN_COMPOSER_H_
#define CXXIME_PINYIN_COMPOSER_H_

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
