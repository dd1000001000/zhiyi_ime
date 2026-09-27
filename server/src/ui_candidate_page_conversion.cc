// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "ui_candidate_page_conversion.h"

#include <algorithm>
#include <utility>

namespace cxxime {
namespace {

std::string packet_text(const char* text, std::uint32_t length, std::size_t capacity) {
    const std::size_t safe_length = (std::min)(static_cast<std::size_t>(length), capacity);
    return std::string(text, text + safe_length);
}

void copy_common_fields(const UiPresentationSnapshot& snapshot, CandidatePage* page) {
    const UiCandidatePage& source = snapshot.candidate_page;
    page->page_index = source.page_current > 0 ? static_cast<int>(source.page_current - 1) : 0;
    page->page_offset = static_cast<int>(source.offset);
    page->page_size = static_cast<int>(source.count);
    page->extent.known_count = static_cast<int>(snapshot.candidate_known_count);
    page->extent.state = snapshot.candidate_extent_state;
    page->extent.complete = snapshot.candidate_extent_complete != 0;
    page->highlighted = source.count > 0 ? static_cast<int>(source.highlighted) : -1;
}

} // namespace

CandidatePage candidate_page_from_snapshot(const UiPresentationSnapshot& snapshot) {
    const UiCandidatePage& source = snapshot.candidate_page;
    CandidatePage page;
    copy_common_fields(snapshot, &page);
    page.candidates.reserve(source.count);
    for (std::uint32_t index = 0; index < source.count; ++index) {
        const UiCandidate& source_candidate = source.candidates[index];
        Candidate candidate;
        candidate.text = packet_text(source_candidate.text, source_candidate.text_length,
                                     sizeof(source_candidate.text));
        candidate.comment = packet_text(source_candidate.hint, source_candidate.hint_length,
                                        sizeof(source_candidate.hint));
        page.candidates.push_back(std::move(candidate));
    }
    return page;
}

} // namespace cxxime
