// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/commit_learning.h>

#include <utility>

#include <cxxime/composition_state.h>
#include <cxxime/user_dict_validation.h>

#include "pinyin_path_filter.h"

namespace cxxime {
namespace {

// The whole sentence of a segmented commit: its text, the letters typed and the syllables.
struct SentenceParts {
    std::string text;
    std::string code;
    std::string syllables;
};

const CandidateCanonicalVariant* primary_variant(const ConvertedSegment& segment) {
    return segment.primary_variant < segment.variants.size()
               ? &segment.variants[segment.primary_variant]
               : nullptr;
}

const CandidateCanonicalVariant* primary_variant(const TextSelectionAction& action) {
    return action.primary_variant < action.variants.size()
               ? &action.variants[action.primary_variant]
               : nullptr;
}

void append_preference(CommitLearningPlan& plan, const std::string& text,
                       const std::string& typed_code, const CandidateCanonicalVariant* variant) {
    if (!variant || variant->learning_target == LearningTarget::kNone ||
        variant->provenance.origin == CandidateOrigin::kComposed) {
        return;
    }
    CandidatePreferenceLearningEvent event;
    event.target = variant->learning_target;
    // The letters typed, without syllable separators ("xi'an" learned as "xian").
    event.typed_code = without_separators(variant->input_code.empty() ? typed_code : variant->input_code);
    event.candidate.text = text;
    event.candidate.code = variant->code;
    event.candidate.syllables = variant->syllables;
    event.candidate.frequency = variant->frequency;
    event.candidate.source_frequency = variant->source_frequency;
    event.candidate.source = variant->provenance.source;
    event.candidate.origin = variant->provenance.origin;
    plan.candidate_preferences.push_back(std::move(event));
}

bool append_sentence_part(SentenceParts& sentence, const std::string& text,
                          const std::string& raw_input, const CandidateCanonicalVariant* variant) {
    if (!variant || variant->learning_target != LearningTarget::kPinyin ||
        variant->provenance.source != CandidateSource::kPinyin || variant->syllables.empty()) {
        return false;
    }
    sentence.text += text;
    sentence.code += without_separators(variant->input_code.empty() ? raw_input : variant->input_code);
    if (!sentence.syllables.empty()) {
        sentence.syllables.push_back(':');
    }
    sentence.syllables += variant->syllables;
    return true;
}

CommitLearningPlan make_learning_plan(const CompositionState& state,
                                      const TextSelectionAction* final_action,
                                      bool include_sentence) {
    CommitLearningPlan plan;
    SentenceParts sentence;
    const CandidateCanonicalVariant* final_variant =
        final_action ? primary_variant(*final_action) : nullptr;
    bool sentence_is_valid =
        include_sentence && state.active().scheme == CompositionScheme::kPinyin &&
        (!state.converted_segments().empty() ||
         (final_variant && final_variant->provenance.origin == CandidateOrigin::kComposed));
    for (const ConvertedSegment& segment : state.converted_segments()) {
        const CandidateCanonicalVariant* variant = primary_variant(segment);
        append_preference(plan, segment.text, segment.raw_input, variant);
        sentence_is_valid =
            append_sentence_part(sentence, segment.text, segment.raw_input, variant) &&
            sentence_is_valid;
    }
    if (final_action) {
        const CandidateCanonicalVariant* variant = primary_variant(*final_action);
        append_preference(plan, final_action->text, state.active().input, variant);
        sentence_is_valid = append_sentence_part(sentence, final_action->text,
                                                 state.active().input, variant) &&
                            sentence_is_valid;
    }
    if (sentence_is_valid && is_valid_user_dict_text(sentence.text) &&
        is_valid_user_dict_code(sentence.code) &&
        is_valid_user_dict_syllables(sentence.syllables)) {
        CandidatePreferenceLearningEvent event;
        event.target = LearningTarget::kPinyin;
        event.typed_code = sentence.code;
        event.candidate.text = std::move(sentence.text);
        event.candidate.code = std::move(sentence.code);
        event.candidate.syllables = std::move(sentence.syllables);
        event.candidate.source = CandidateSource::kPinyin;
        event.candidate.origin = CandidateOrigin::kComposed;
        plan.candidate_preferences.push_back(std::move(event));
    }
    return plan;
}

} // namespace

CommitLearningPlan make_candidate_learning_plan(const CompositionState& state,
                                                const TextSelectionAction& final_action) {
    return make_learning_plan(state, &final_action, true);
}

CommitLearningPlan make_raw_learning_plan(const CompositionState& state) {
    return make_learning_plan(state, nullptr, false);
}

CommitLearningPlan make_partial_raw_learning_plan(const CompositionState& state,
                                                  const TextSelectionAction& partial_action) {
    return make_learning_plan(state, &partial_action, false);
}

} // namespace cxxime
