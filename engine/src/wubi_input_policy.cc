// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/wubi_input_policy.h>

#include <cxxime/input_limits.h>

namespace cxxime {

namespace {

bool is_plain_letter(uint32_t keycode) { return keycode >= 'A' && keycode <= 'Z'; }

bool first_candidate_is_wubi(const TranslationResult& result) {
    return !result.entries.empty() &&
           result.entries.front().candidate.source == CandidateSource::kWubi;
}

} // namespace

WubiFifthKeyAction WubiInputPolicy::fifth_key_action(CompositionScheme scheme,
                                                     const std::string& active_input,
                                                     std::size_t cursor,
                                                     const TranslationResult& result,
                                                     const Config& config,
                                                     uint32_t keycode) {
    if (!is_plain_letter(keycode) || active_input.size() != kMaxWubiCodeLength ||
        cursor != active_input.size()) {
        return WubiFifthKeyAction::kNone;
    }
    const bool wubi_mode = scheme == CompositionScheme::kWubi;
    const bool mixed_mode = scheme == CompositionScheme::kMixed;
    const bool multiple_candidates = result.extent.known_count > 1 ||
                                     result.extent.state != CandidateExtentState::kExhausted;
    if (config.wubi_commit_first_on_fifth_key && (wubi_mode || mixed_mode) &&
        multiple_candidates && first_candidate_is_wubi(result)) {
        return WubiFifthKeyAction::kCommitFirstAndRestart;
    }
    if (config.wubi_restart_on_fifth_after_miss && wubi_mode && result.entries.empty() &&
        result.extent.known_count == 0 &&
        result.extent.state == CandidateExtentState::kExhausted) {
        return WubiFifthKeyAction::kRestartAfterMiss;
    }
    return WubiFifthKeyAction::kNone;
}

bool WubiInputPolicy::should_auto_commit(CompositionScheme scheme,
                                         const std::string& active_input,
                                         const TranslationResult& result,
                                         const Config& config) {
    return config.wubi_auto_commit &&
           (scheme == CompositionScheme::kWubi || scheme == CompositionScheme::kMixed) &&
           active_input.size() == kMaxWubiCodeLength && result.entries.size() == 1 &&
           result.extent.known_count == 1 &&
           result.extent.state == CandidateExtentState::kExhausted &&
           first_candidate_is_wubi(result);
}

} // namespace cxxime
