// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_CANDIDATE_H_
#define CXXIME_CANDIDATE_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <cxxime/input_limits.h>

namespace cxxime {

enum class CandidateSource {
    kPinyin,
    kWubi,
    kSymbol,
    kEnglish,  // Laya: English word candidates (EnglishLexicon)
};

enum class CandidateOrigin {
    kSystem,
    kUser,
    kLearned,
    kCache,
    kComposed,
};

struct Candidate {
    std::string text;
    std::string comment;
    int frequency = 0;
    CandidateSource source = CandidateSource::kPinyin;
    std::string code;
    std::string syllables;
    CandidateOrigin origin = CandidateOrigin::kSystem;
    int source_frequency = 0;  // Raw dictionary frequency when ranking uses a derived score.
    // Canonical input used to query this candidate when it differs from the user's raw keys.
    std::string input_code;
    // Picked by the Laya model as the best continuation (shown with a sparkle mark).
    bool recommended = false;
    bool learned = false;  // from the user's own selections (self-learning): a small dot mark
};

enum class CandidateExtentState : std::uint32_t {
    kExhausted = 0,
    kHasMore = 1,
    kIndeterminate = 2,
};

struct CandidateExtent {
    int known_count = 0;
    CandidateExtentState state = CandidateExtentState::kExhausted;
    bool complete = true;
};

inline CandidateExtent make_candidate_extent(int known_count, int returned_end, bool incomplete) {
    CandidateExtent extent;
    extent.known_count = (std::max)(0, (std::max)(known_count, returned_end));
    extent.complete = !incomplete;
    if (extent.known_count > returned_end) {
        extent.state = CandidateExtentState::kHasMore;
    } else if (incomplete) {
        extent.state = CandidateExtentState::kIndeterminate;
    }
    return extent;
}

inline bool candidate_extent_may_continue(const CandidateExtent& extent, int returned_end) {
    return extent.known_count > returned_end || extent.state != CandidateExtentState::kExhausted;
}

inline int legacy_candidate_total(const CandidateExtent& extent, int returned_end) {
    const int continuation_count =
        extent.state == CandidateExtentState::kExhausted ? returned_end : returned_end + 1;
    return (std::max)(extent.known_count, continuation_count);
}

inline bool candidate_text_fits(const std::string& text) {
    return text.size() < kCandidateTextCapacity;
}

inline const std::string& candidate_display_text(const Candidate& candidate,
                                                 std::string& formatted) {
    if (candidate.comment.empty()) {
        return candidate.text;
    }
    formatted.clear();
    formatted.reserve(candidate.text.size() + candidate.comment.size() + 2);
    formatted.append(candidate.text);
    formatted.push_back('(');
    formatted.append(candidate.comment);
    formatted.push_back(')');
    return formatted;
}

struct CandidatePage {
    int page_index = 0;
    int page_offset = 0;
    int page_size = 9;
    CandidateExtent extent;
    int highlighted = -1;
    std::vector<Candidate> candidates;
};

// Candidate page cache with user dictionary version for invalidation.
struct CachedCandidatePage {
    uint64_t user_dict_version = 0;
    CandidatePage page;
};

} // namespace cxxime

#endif // CXXIME_CANDIDATE_H_
