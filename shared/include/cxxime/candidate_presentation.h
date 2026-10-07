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
    bool learned = false;      // from self-learning
    std::string gloss;         // learning mode translation (encoded, see below)
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

// Learning mode translations (docs/learning-mode.md) travel as one string per candidate: senses
// "label\x1Ftext" joined by '\x1E', e.g. "n.\x1Fnow\x1Eadv.\x1Fat present".
inline constexpr char kGlossSenseSeparator = '\x1E';
inline constexpr char kGlossLabelSeparator = '\x1F';

struct GlossPart {
    std::string label;  // "n.", "v."; may be empty
    std::string text;
};

inline std::vector<GlossPart> decode_candidate_gloss(const std::string& gloss) {
    std::vector<GlossPart> parts;
    std::size_t start = 0;
    while (start < gloss.size()) {
        std::size_t end = gloss.find(kGlossSenseSeparator, start);
        if (end == std::string::npos) end = gloss.size();
        const std::string sense = gloss.substr(start, end - start);
        const std::size_t label_end = sense.find(kGlossLabelSeparator);
        GlossPart part;
        if (label_end == std::string::npos) {
            part.text = sense;
        } else {
            part.label = sense.substr(0, label_end);
            part.text = sense.substr(label_end + 1);
        }
        if (!part.text.empty()) parts.push_back(std::move(part));
        start = end + 1;
    }
    return parts;
}

// The whole senses that fit in `capacity` bytes with a terminator.
inline std::string fit_candidate_gloss(const std::string& gloss, std::size_t capacity) {
    if (gloss.size() < capacity) return gloss;
    const std::size_t end = gloss.rfind(kGlossSenseSeparator, capacity - 1);
    return end == std::string::npos ? std::string() : gloss.substr(0, end);
}

} // namespace cxxime

#endif // CXXIME_CANDIDATE_PRESENTATION_H_
