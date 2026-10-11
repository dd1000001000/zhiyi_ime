// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_COMMIT_LEARNING_H_
#define CXXIME_COMMIT_LEARNING_H_

#include <string>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/candidate_selection.h>

namespace cxxime {

class CompositionState;

struct CandidatePreferenceLearningEvent {
    LearningTarget target = LearningTarget::kNone;
    Candidate candidate;
    std::string typed_code;
};

// What one commit teaches the candidate preferences (candidate-ordering.md): each picked word
// under the letters typed for it, and a pinyin sentence picked segment by segment or committed
// as a composed sentence as one more entry (origin kComposed: a sentence in
// learning_pinyin.tsv).
struct CommitLearningPlan {
    std::vector<CandidatePreferenceLearningEvent> candidate_preferences;

    bool empty() const { return candidate_preferences.empty(); }
};

CommitLearningPlan make_candidate_learning_plan(const CompositionState& state,
                                                const TextSelectionAction& final_action);
CommitLearningPlan make_raw_learning_plan(const CompositionState& state);
CommitLearningPlan make_partial_raw_learning_plan(const CompositionState& state,
                                                  const TextSelectionAction& partial_action);

} // namespace cxxime

#endif // CXXIME_COMMIT_LEARNING_H_
