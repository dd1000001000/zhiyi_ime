// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TRANSLATION_RESULT_H_
#define CXXIME_TRANSLATION_RESULT_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/candidate_presentation.h>
#include <cxxime/candidate_selection.h>
#include <cxxime/translation_policy.h>

namespace cxxime {

struct QueryBudget;
struct QueryScratch;
struct QueryTrace;

enum class TranslationStatus {
    kSuccess,
    // A stable candidate prefix exists, but the requested extent could not be completed.
    kStableDegraded,
    kFailed,
};

struct TranslationRequest {
    CompositionScheme scheme = CompositionScheme::kPinyin;
    std::string input;
    int page_index = 0;
    int page_offset = 0;
    int page_size = 9;
    TranslationPolicy policy;
    QueryTrace* trace = nullptr;
    const QueryBudget* budget = nullptr;
    QueryScratch* scratch = nullptr;
};

struct CandidateEntry {
    Candidate candidate;
    std::string hint;
    CandidateSelection selection;
};

inline bool same_candidate_entry_identity(const CandidateEntry& left, const CandidateEntry& right) {
    return left.candidate.text == right.candidate.text &&
           same_selection_action(left.selection, right.selection);
}

struct TranslationResult {
    TranslationStatus status = TranslationStatus::kSuccess;
    int page_index = 0;
    int page_offset = 0;
    int page_size = 9;
    CandidateExtent extent;
    int highlighted = -1;
    std::vector<CandidateEntry> entries;

    bool usable() const { return status != TranslationStatus::kFailed; }

    CandidatePage candidate_page() const {
        CandidatePage page;
        page.page_index = page_index;
        page.page_offset = page_offset;
        page.page_size = page_size;
        page.extent = extent;
        page.extent.known_count = (std::max)(
            page.extent.known_count,
            page.page_offset + static_cast<int>(entries.size()));
        page.highlighted = highlighted;
        page.candidates.reserve(entries.size());
        for (const auto& entry : entries) {
            Candidate candidate = entry.candidate;
            candidate.comment = entry.hint;
            page.candidates.push_back(std::move(candidate));
        }
        return page;
    }

    CandidatePresentationPage presentation_page() const {
        CandidatePresentationPage page;
        page.page_index = page_index;
        page.page_offset = page_offset;
        page.page_size = page_size;
        page.extent = extent;
        page.extent.known_count = (std::max)(
            page.extent.known_count,
            page.page_offset + static_cast<int>(entries.size()));
        page.highlighted = highlighted;
        page.items.reserve(entries.size());
        for (const auto& entry : entries) {
            page.items.push_back({entry.candidate.text, entry.hint, entry.candidate.recommended,
                                  entry.candidate.learned});
        }
        return page;
    }
};

inline CandidateCanonicalVariant canonical_variant(const Candidate& candidate,
                                                   const std::string& input_code = {}) {
    CandidateCanonicalVariant variant;
    variant.provenance = {candidate.source, candidate.origin};
    variant.input_code = input_code.empty() ? candidate.input_code : input_code;
    variant.code = candidate.code;
    variant.syllables = candidate.syllables;
    variant.frequency = candidate.frequency;
    variant.source_frequency = candidate.source_frequency;
    if (candidate.source == CandidateSource::kPinyin) {
        variant.learning_target = LearningTarget::kPinyin;
    } else if (candidate.source == CandidateSource::kWubi) {
        variant.learning_target = LearningTarget::kWubi;
    }
    return variant;
}

inline CandidateEntry make_text_candidate_entry(Candidate candidate,
                                                std::size_t consumed_input_bytes,
                                                const std::string& input_code = {}) {
    CandidateEntry entry;
    entry.hint = candidate.comment;
    TextSelectionAction action;
    action.text = candidate.text;
    action.consumed_input_bytes = consumed_input_bytes;
    action.variants.push_back(canonical_variant(candidate, input_code));
    entry.candidate = std::move(candidate);
    entry.selection = std::move(action);
    return entry;
}

inline TranslationResult make_translation_result(CandidatePage page,
                                                 std::size_t consumed_input_bytes) {
    TranslationResult result;
    result.page_index = page.page_index;
    result.page_offset = page.page_offset;
    result.page_size = page.page_size;
    result.extent = page.extent;
    result.extent.known_count = (std::max)(
        result.extent.known_count,
        result.page_offset + static_cast<int>(page.candidates.size()));
    result.highlighted = page.highlighted;
    result.entries.reserve(page.candidates.size());
    for (auto& candidate : page.candidates) {
        result.entries.push_back(
            make_text_candidate_entry(std::move(candidate), consumed_input_bytes));
    }
    return result;
}

} // namespace cxxime

#endif // CXXIME_TRANSLATION_RESULT_H_
