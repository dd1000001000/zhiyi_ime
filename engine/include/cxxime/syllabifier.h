// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Syllabifier — builds syllable graph from input using SpellingsIndex.
// Corresponds to librime Syllabifier (algo/syllabifier.cc) BuildSyllableGraph.

#ifndef CXXIME_SYLLABIFIER_H_
#define CXXIME_SYLLABIFIER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cxxime/spellings_index.h>

namespace cxxime {

class PinyinResourceSet;

struct QueryDeadline;  // forward declaration

// A possible syllable at a graph edge
struct SyllableEdge {
    std::string syllable;
    int type = kNormalSpelling;
    float credibility = 0.0f;
};

// SyllableGraph: edges[start_pos] = map<end_pos, list<SyllableEdge>>
// Corresponds to librime SyllableGraph edges.
using SyllableGraph = std::map<size_t, std::map<size_t, std::vector<SyllableEdge>>>;

// A syllable segmentation path
using SyllablePath = std::vector<std::string>;

struct SegmentedPath {
    SyllablePath syllables;
    std::vector<uint8_t> spelling_types;
    std::vector<uint16_t> input_lengths;
    float credibility = 0.0f;
};

// Syllabifier segmentation result, including deadline status.
struct SegmentResult {
    std::vector<SegmentedPath> paths;
    bool truncated = false;
    bool deadline_exceeded = false;
};

// Initials mode: true when `syllables` ("zhong:guo:ren") has exactly one syllable per typed
// initial, each typed as its first letter or, for zh/ch/sh, as the two letters ("zgr" and
// "zhgr" both match zhong:guo:ren).
bool pinyin_matches_initials(std::string_view input, std::string_view syllables);

struct SyllabifierOptions {
    bool enable_fuzzy = true;
    // Initials mode: only one-letter spans (and zh/ch/sh) are syllables, as initials.
    bool initials_only = false;
    bool enable_terminal_completion = false;
    bool collect_path_metadata = false;
};

class Syllabifier {
public:
    Syllabifier(const Syllabifier&) = delete;
    Syllabifier& operator=(const Syllabifier&) = delete;
    Syllabifier(Syllabifier&&) = delete;
    Syllabifier& operator=(Syllabifier&&) = delete;

    // Build syllable graph from input.
    // Corresponds to librime BuildSyllableGraph.
    SyllableGraph build_graph(const std::string& input,
                              const SyllabifierOptions& options = {}) const;

    // Segment input into syllable paths, sorted by quality.
    // Best (all-normal) paths first, then fuzzy, then abbreviation.
    // Optional deadline for internal checks during DFS.
    SegmentResult segment(const std::string& input, const QueryDeadline* deadline = nullptr,
                          const SyllabifierOptions& options = {}) const;
    bool has_fuzzy_path(const std::string& input,
                        const SyllabifierOptions& options = {}) const;

private:
    friend class PinyinResourceSet;

    explicit Syllabifier(const SpellingsIndex& spellings);

    const SpellingsIndex& spellings_;

    // DFS enumeration of all paths through the graph
    // Returns true if the deadline expired during enumeration.
    bool enumerate_paths(const SyllableGraph& graph,
                         size_t pos, size_t end_pos,
                         SegmentedPath& current,
                         std::vector<SegmentedPath>& results,
                         const QueryDeadline* deadline,
                         bool collect_path_metadata,
                         uint32_t& path_count,
                         std::vector<std::pair<size_t, std::vector<SyllableEdge>>>& sorted_scratch,
                         uint32_t& call_count) const;
};

} // namespace cxxime

#endif // CXXIME_SYLLABIFIER_H_
