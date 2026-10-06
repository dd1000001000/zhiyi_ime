// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_CANDIDATE_PRESENTATION_H_
#define CXXIME_CANDIDATE_PRESENTATION_H_

#include <string>
#include <vector>

#include <cxxime/candidate.h>

namespace cxxime {

struct CandidatePresentationItem {
    std::string text;
    std::string hint;
    bool recommended = false;  // Laya's pick (sparkle mark)
    bool learned = false;      // self-learning (dot mark; Ctrl+Delete forgets it)
};

struct CandidatePresentationPage {
    int page_index = 0;
    int page_offset = 0;
    int page_size = 9;
    CandidateExtent extent;
    int highlighted = -1;
    std::vector<CandidatePresentationItem> items;
};

inline std::string format_candidate_presentation(const CandidatePresentationItem& item) {
    std::string formatted = item.text;
    if (!item.hint.empty()) {
        formatted.append("(").append(item.hint).append(")");
    }
    return formatted;
}

} // namespace cxxime

#endif // CXXIME_CANDIDATE_PRESENTATION_H_
