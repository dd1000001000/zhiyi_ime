// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "pinyin_partial_candidates.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <cxxime/dict.h>
#include <cxxime/pinyin_resource.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_trace.h>

#include "pinyin_path_filter.h"
#include "pinyin_query_key.h"

namespace cxxime {

namespace {

struct PartialCandidate {
    std::size_t consumed = 0;
    std::string lookup_key;
    CandidateEntry entry;
};

struct PartialBoundary {
    std::size_t consumed = 0;
    std::string canonical_key;
    std::vector<uint32_t> ids;
    int worst_spelling_type = kNormalSpelling;
    float credibility = 0.0f;
};

constexpr std::size_t kMaxAbbreviationPaths = 16;
bool is_natural_path(const std::string& input, const SegmentedPath& path) {
    return path_consumes_entire_input(input, path) &&
           std::all_of(path.spelling_types.begin(), path.spelling_types.end(),
                       [](uint8_t type) { return type <= kFuzzySpelling; });
}

bool same_boundary(const PartialBoundary& left, const PartialBoundary& right) {
    return left.consumed == right.consumed && left.canonical_key == right.canonical_key &&
           left.ids == right.ids;
}

void append_path_boundaries(Dict& dict, const SegmentedPath& path,
                            std::vector<PartialBoundary>& boundaries) {
    std::vector<uint32_t> ids;
    ids.reserve(path.syllables.size());
    for (const std::string& syllable : path.syllables) {
        const uint32_t id = dict.syllable_to_id(syllable);
        if (id == UINT32_MAX) {
            return;
        }
        ids.push_back(id);
    }

    std::size_t consumed = 0;
    int worst_spelling_type = kNormalSpelling;
    for (std::size_t end = 0; end + 1 < ids.size(); ++end) {
        consumed += path.input_lengths[end];
        worst_spelling_type =
            (std::max)(worst_spelling_type, static_cast<int>(path.spelling_types[end]));
        PartialBoundary boundary;
        boundary.consumed = consumed;
        boundary.canonical_key = canonical_pinyin_key(path.syllables, end + 1);
        boundary.ids.assign(ids.begin(), ids.begin() + end + 1);
        boundary.worst_spelling_type = worst_spelling_type;
        boundary.credibility = path.credibility;

        const auto existing =
            std::find_if(boundaries.begin(), boundaries.end(), [&](const PartialBoundary& item) {
                return same_boundary(item, boundary);
            });
        if (existing == boundaries.end()) {
            boundaries.push_back(std::move(boundary));
        } else if (boundary.worst_spelling_type < existing->worst_spelling_type ||
                   (boundary.worst_spelling_type == existing->worst_spelling_type &&
                    boundary.credibility > existing->credibility)) {
            *existing = std::move(boundary);
        }
    }
}

std::vector<PartialBoundary> collect_partial_boundaries(Dict& dict, const SegmentResult& segmented,
                                                        const std::string& input) {
    const bool has_natural_path =
        std::any_of(segmented.paths.begin(), segmented.paths.end(),
                    [&](const SegmentedPath& path) { return is_natural_path(input, path); });

    std::vector<PartialBoundary> boundaries;
    std::size_t abbreviation_path_count = 0;
    for (const SegmentedPath& path : segmented.paths) {
        if (!path_consumes_entire_input(input, path) || path.syllables.size() < 2) {
            continue;
        }
        if (has_natural_path && !is_natural_path(input, path)) {
            continue;
        }
        if (!has_natural_path && abbreviation_path_count++ >= kMaxAbbreviationPaths) {
            break;
        }
        append_path_boundaries(dict, path, boundaries);
    }

    std::sort(boundaries.begin(), boundaries.end(),
              [](const PartialBoundary& left, const PartialBoundary& right) {
                  if (left.worst_spelling_type != right.worst_spelling_type) {
                      return left.worst_spelling_type < right.worst_spelling_type;
                  }
                  if (left.credibility != right.credibility) {
                      return left.credibility > right.credibility;
                  }
                  if (left.consumed != right.consumed) {
                      return left.consumed > right.consumed;
                  }
                  return left.ids < right.ids;
              });
    return boundaries;
}

bool same_candidate_identity(const Candidate& left, const Candidate& right) {
    return left.text == right.text && left.code == right.code && left.syllables == right.syllables;
}

void merge_or_append_partial(std::vector<PartialCandidate>& partials,
                             Candidate candidate,
                             std::size_t consumed,
                             const std::string& lookup_key,
                             bool include_input_code,
                             std::size_t input_size) {
    if (consumed == 0 || consumed >= input_size || !candidate_text_fits(candidate.text)) {
        return;
    }
    candidate.source = CandidateSource::kPinyin;
    CandidateEntry entry = make_text_candidate_entry(
        std::move(candidate), consumed, include_input_code ? lookup_key : std::string{});
    const auto existing =
        std::find_if(partials.begin(), partials.end(), [&](const PartialCandidate& item) {
            return item.consumed == consumed && item.lookup_key == lookup_key &&
                   item.entry.candidate.text == entry.candidate.text;
        });
    if (existing == partials.end()) {
        partials.push_back({consumed, lookup_key, std::move(entry)});
        return;
    }
    if (entry.candidate.frequency > existing->entry.candidate.frequency) {
        auto& replacement_action = std::get<TextSelectionAction>(entry.selection);
        const auto& previous_action = std::get<TextSelectionAction>(existing->entry.selection);
        merge_candidate_variants(replacement_action, previous_action);
        existing->entry = std::move(entry);
    } else {
        auto& existing_action = std::get<TextSelectionAction>(existing->entry.selection);
        const auto& action = std::get<TextSelectionAction>(entry.selection);
        merge_candidate_variants(existing_action, action);
    }
}

void rank_partial_candidates(Dict& dict,
                             bool candidate_learning_enabled,
                             bool include_input_code,
                             std::vector<PartialCandidate>& partials,
                             int limit) {
    std::vector<std::pair<std::size_t, std::string>> group_keys;
    group_keys.reserve(partials.size());
    for (const auto& partial : partials) {
        const auto key = std::make_pair(partial.consumed, partial.lookup_key);
        if (std::find(group_keys.begin(), group_keys.end(), key) == group_keys.end()) {
            group_keys.push_back(key);
        }
    }
    std::sort(group_keys.begin(), group_keys.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first > right.first;
        }
        return left.second < right.second;
    });

    std::vector<std::vector<PartialCandidate>> groups;
    groups.reserve(group_keys.size());
    for (const auto& group_key : group_keys) {
        const std::size_t consumed = group_key.first;
        const std::string& lookup_key = group_key.second;
        std::vector<Candidate> candidates;
        for (const auto& partial : partials) {
            if (partial.consumed == consumed && partial.lookup_key == lookup_key) {
                candidates.push_back(partial.entry.candidate);
            }
        }

        dict.filter_disabled_system_candidates(candidates);
        if (candidate_learning_enabled) {
            dict.apply_candidate_preferences(lookup_key, CandidateSource::kPinyin,
                                             candidates, limit);
        }
        dict.apply_manual_candidate_order(lookup_key, CandidateSource::kPinyin,
                                          candidates, limit);

        std::vector<PartialCandidate> group;
        group.reserve(candidates.size());
        for (auto& candidate : candidates) {
            CandidateEntry entry = make_text_candidate_entry(
                candidate, consumed, include_input_code ? lookup_key : std::string{});
            const auto original =
                std::find_if(partials.begin(), partials.end(),
                             [&](const PartialCandidate& item) {
                                 return item.consumed == consumed &&
                                        item.lookup_key == lookup_key &&
                                        same_candidate_identity(item.entry.candidate, candidate);
                             });
            if (original != partials.end()) {
                auto& action = std::get<TextSelectionAction>(entry.selection);
                const auto& original_action =
                    std::get<TextSelectionAction>(original->entry.selection);
                merge_candidate_variants(action, original_action);
            }
            group.push_back({consumed, lookup_key, std::move(entry)});
        }
        groups.push_back(std::move(group));
    }

    std::vector<PartialCandidate> ranked;
    ranked.reserve(partials.size());
    for (std::size_t rank = 0;; ++rank) {
        bool appended = false;
        for (auto& group : groups) {
            if (rank < group.size()) {
                ranked.push_back(std::move(group[rank]));
                appended = true;
            }
        }
        if (!appended) {
            break;
        }
    }
    partials = std::move(ranked);
}

void merge_or_append_visible_candidate(std::vector<CandidateEntry>& entries,
                                       CandidateEntry candidate, std::size_t input_size) {
    const auto existing =
        std::find_if(entries.begin(), entries.end(), [&](const CandidateEntry& entry) {
            return entry.candidate.text == candidate.candidate.text;
        });
    if (existing == entries.end()) {
        entries.push_back(std::move(candidate));
        return;
    }
    if (same_selection_action(existing->selection, candidate.selection)) {
        auto* existing_action = std::get_if<TextSelectionAction>(&existing->selection);
        const auto* candidate_action = std::get_if<TextSelectionAction>(&candidate.selection);
        if (existing_action && candidate_action) {
            merge_candidate_variants(*existing_action, *candidate_action);
        }
        return;
    }
    if (should_prefer_visible_selection(candidate.selection, existing->selection, input_size)) {
        const auto* existing_action = std::get_if<TextSelectionAction>(&existing->selection);
        const auto* candidate_action = std::get_if<TextSelectionAction>(&candidate.selection);
        const bool moves_from_full_to_partial =
            existing_action && candidate_action &&
            existing_action->consumed_input_bytes == input_size &&
            candidate_action->consumed_input_bytes < input_size;
        if (moves_from_full_to_partial) {
            entries.erase(existing);
            entries.push_back(std::move(candidate));
            return;
        }
        *existing = std::move(candidate);
    }
}

} // namespace

void append_pinyin_partial_candidates(Dict& dict,
                                      const PinyinResourceSet& pinyin_resources,
                                      PinyinQueryPolicy pinyin_query_policy,
                                      const TranslationRequest& request,
                                      bool shuangpin,
                                      bool candidate_learning_enabled,
                                      std::vector<CandidateEntry>& entries,
                                      TranslationStatus& status) {
    if (!request.policy.allow_partial_selection || request.input.size() < 2) {
        return;
    }

    const QueryDeadline* deadline = request.budget ? &request.budget->deadline : nullptr;
    SyllabifierOptions options;
    options.enable_fuzzy = pinyin_query_policy.enable_fuzzy;
    options.enable_terminal_completion = shuangpin;
    options.collect_path_metadata = true;
    const SegmentResult segmented = pinyin_resources.segment(request.input, deadline, options);
    if (segmented.deadline_exceeded) {
        status = status == TranslationStatus::kFailed ? TranslationStatus::kFailed
                                                      : TranslationStatus::kStableDegraded;
        return;
    }
    if (segmented.truncated) {
        status = status == TranslationStatus::kFailed ? TranslationStatus::kFailed
                                                      : TranslationStatus::kStableDegraded;
    }

    const std::vector<PartialBoundary> boundaries =
        collect_partial_boundaries(dict, segmented, request.input);
    std::vector<PartialCandidate> partials;
    SpanLookupStats span_stats;
    const QueryDeadline no_deadline;
    const QueryDeadline& effective_deadline = deadline ? *deadline : no_deadline;
    const QueryBudget default_budget;
    const QueryBudget& effective_budget = request.budget ? *request.budget : default_budget;
    SpanLookupLimits limits;
    limits.max_candidates_per_range =
        static_cast<uint32_t>(kMaxSegmentedPartialCandidateCount);
    limits.max_results = static_cast<uint32_t>(kMaxSegmentedPartialCandidateCount);
    UserLookupStats user_stats;
    bool span_scan_budget_truncated = false;

    std::vector<std::pair<std::size_t, std::string>> queried_user_prefixes;
    for (const PartialBoundary& boundary : boundaries) {
        const std::string lookup_key = shuangpin
                                           ? boundary.canonical_key
                                           : request.input.substr(0, boundary.consumed);
        const auto user_prefix = std::make_pair(boundary.consumed, lookup_key);
        if (span_stats.entry_scans >= effective_budget.max_exact_scan) {
            span_stats.truncated = true;
            span_scan_budget_truncated = true;
            break;
        }

        SpanLookupStats boundary_stats;
        limits.max_entry_scans = effective_budget.max_exact_scan - span_stats.entry_scans;
        std::vector<Candidate> candidates;
        dict.lookup_exact_span(boundary.ids, 0, boundary.ids.size(), limits,
                               effective_deadline, candidates, boundary_stats);
        span_stats.range_queries += boundary_stats.range_queries;
        span_stats.entry_scans += boundary_stats.entry_scans;
        span_stats.result_count += boundary_stats.result_count;
        span_stats.truncated = span_stats.truncated || boundary_stats.truncated;
        span_stats.deadline_exceeded =
            span_stats.deadline_exceeded || boundary_stats.deadline_exceeded;
        if (boundary_stats.truncated && !boundary_stats.deadline_exceeded &&
            boundary_stats.entry_scans >= limits.max_entry_scans) {
            span_scan_budget_truncated = true;
        }
        for (auto& candidate : candidates) {
            if (pinyin_query_policy.initials_only &&
                !pinyin_matches_initials(request.input.substr(0, boundary.consumed),
                                         candidate.syllables)) {
                continue;
            }
            if (!dict.is_system_entry_disabled(candidate.text)) {
                merge_or_append_partial(partials, std::move(candidate), boundary.consumed,
                                        lookup_key, shuangpin,
                                        request.input.size());
            }
        }

        if (std::find(queried_user_prefixes.begin(), queried_user_prefixes.end(), user_prefix) ==
            queried_user_prefixes.end()) {
            queried_user_prefixes.push_back(user_prefix);
            std::vector<Candidate> user_candidates = dict.lookup_user_exact(
                lookup_key, static_cast<int>(limits.max_candidates_per_range),
                effective_budget, nullptr, &user_stats);
            for (auto& candidate : user_candidates) {
                if (pinyin_query_policy.initials_only &&
                    !pinyin_matches_initials(lookup_key, candidate.syllables)) {
                    continue;
                }
                merge_or_append_partial(partials, std::move(candidate), boundary.consumed,
                                        lookup_key, shuangpin,
                                        request.input.size());
            }
        }

        if (span_stats.deadline_exceeded || user_stats.deadline_exceeded ||
            user_stats.truncated) {
            status = status == TranslationStatus::kFailed ? TranslationStatus::kFailed
                                                          : TranslationStatus::kStableDegraded;
            break;
        }
    }

    if (span_stats.truncated || user_stats.truncated) {
        status = status == TranslationStatus::kFailed ? TranslationStatus::kFailed
                                                        : TranslationStatus::kStableDegraded;
    }

    rank_partial_candidates(dict, candidate_learning_enabled, shuangpin, partials,
                            static_cast<int>(limits.max_candidates_per_range));
    if (partials.size() > kMaxSegmentedPartialCandidateCount) {
        partials.resize(kMaxSegmentedPartialCandidateCount);
    }
    for (auto& partial : partials) {
        merge_or_append_visible_candidate(entries, std::move(partial.entry), request.input.size());
    }
    if (request.trace) {
        request.trace->span_query_count += span_stats.range_queries;
        request.trace->span_entry_scan_count += span_stats.entry_scans;
        request.trace->user_scan_count += user_stats.scan_count;
        request.trace->truncated = request.trace->truncated || span_stats.truncated ||
                                   user_stats.truncated || segmented.truncated;
        request.trace->scan_budget_truncated =
            request.trace->scan_budget_truncated || span_scan_budget_truncated ||
            user_stats.scan_budget_truncated;
        request.trace->deadline_exceeded = request.trace->deadline_exceeded ||
                                           span_stats.deadline_exceeded ||
                                           user_stats.deadline_exceeded;
    }
}

} // namespace cxxime
