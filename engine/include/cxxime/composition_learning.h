// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_COMPOSITION_LEARNING_H_
#define CXXIME_COMPOSITION_LEARNING_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/candidate_selection.h>
#include <cxxime/user_data_merge.h>

namespace cxxime {

class CompositionState;

struct CandidatePreferenceLearningEvent {
    LearningTarget target = LearningTarget::kNone;
    Candidate candidate;
    std::string typed_code;
};

struct CompositionLearningEvent {
    std::string text;
    std::string code;
    std::string syllables;
};

struct CommitLearningPlan {
    std::vector<CandidatePreferenceLearningEvent> candidate_preferences;
    std::optional<CompositionLearningEvent> composition;

    bool empty() const {
        return candidate_preferences.empty() && !composition;
    }
};

CommitLearningPlan make_candidate_learning_plan(const CompositionState& state,
                                                const TextSelectionAction& final_action);
CommitLearningPlan make_raw_learning_plan(const CompositionState& state);
CommitLearningPlan make_partial_raw_learning_plan(const CompositionState& state,
                                                  const TextSelectionAction& partial_action);

class CompositionLearningService {
public:
    using WriteCallback =
        std::function<bool(const std::string& path, const std::string& contents)>;

    explicit CompositionLearningService(WriteCallback write_callback = {});
    ~CompositionLearningService();

    CompositionLearningService(const CompositionLearningService&) = delete;
    CompositionLearningService& operator=(const CompositionLearningService&) = delete;

    bool load(const std::string& path);
    bool start();
    static bool validate_contents(const std::string& contents);
    bool enqueue(const CompositionLearningEvent& event);
    bool flush();
    bool freeze_and_stop();
    bool merge_contents_and_save(const std::string& imported, UserDataMergeResult* result);
    // Forgets every learned word and saves the empty file (settings: clear learning data).
    bool clear_and_save();

    std::vector<Candidate> lookup_candidates(const std::string& code,
                                             std::size_t limit) const;
    std::uint64_t version() const;
    std::size_t entry_count() const;
    std::size_t pending_count() const;

    static constexpr std::size_t kMaxRecordCount = 1024;
    static constexpr std::uint64_t kMaxFileSize = 4ULL * 1024ULL * 1024ULL;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cxxime

#endif // CXXIME_COMPOSITION_LEARNING_H_
