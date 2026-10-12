// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/dict.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxime/extension_lexicon.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_trace.h>

#include "binary_format.h"
#include "wubi_prefix_index.h"

namespace cxxime {

namespace {

constexpr std::size_t kDisabledEntryOverfetch = 16;

bool candidate_better(const Candidate& left, const Candidate& right, size_t prefix_length,
                      bool use_prefix_index) {
    if (!use_prefix_index && left.origin == CandidateOrigin::kSystem &&
        right.origin == CandidateOrigin::kSystem) {
        if (prefix_length > 0) {
            const bool left_exact = left.code.size() == prefix_length;
            const bool right_exact = right.code.size() == prefix_length;
            if (left_exact != right_exact) {
                return left_exact;
            }
            if (left.code.size() != right.code.size()) {
                return left.code.size() < right.code.size();
            }
        }
        if (left.source_frequency != right.source_frequency) {
            return left.source_frequency > right.source_frequency;
        }
    } else if (left.frequency != right.frequency) {
        return left.frequency > right.frequency;
    }

    if (left.code != right.code) {
        return left.code < right.code;
    }
    if (left.text.size() != right.text.size()) {
        return left.text.size() < right.text.size();
    }
    return left.text < right.text;
}

void merge_candidate(std::vector<Candidate>& candidates, Candidate candidate,
                     size_t prefix_length, bool use_prefix_index) {
    for (auto& existing : candidates) {
        if (existing.text == candidate.text) {
            if (candidate_better(candidate, existing, prefix_length, use_prefix_index)) {
                existing = std::move(candidate);
            }
            return;
        }
    }
    candidates.push_back(std::move(candidate));
}

} // namespace

std::vector<Candidate> Dict::lookup(const std::string& code_prefix, int limit, QueryTrace* trace) {
    std::vector<Candidate> results;
    if (!dict_entries_ || limit <= 0) {
        return results;
    }

    const uint32_t prefix_length = static_cast<uint32_t>(code_prefix.size());
    const char* prefix_data = code_prefix.data();
    auto code_view = [&](uint32_t index) {
        const auto& entry = dict_entries_[index];
        return std::string_view(dict_strings_ + entry.syllable_ids_offset, entry.syllable_ids_len);
    };
    auto text_view = [&](uint32_t index) {
        const auto& entry = dict_entries_[index];
        return std::string_view(dict_strings_ + entry.text_offset, entry.text_len);
    };
    auto entry_better = [&](uint32_t left_index, uint32_t right_index) {
        const auto& left = dict_entries_[left_index];
        const auto& right = dict_entries_[right_index];
        if (prefix_length > 0) {
            const bool left_exact = left.syllable_ids_len == prefix_length;
            const bool right_exact = right.syllable_ids_len == prefix_length;
            if (left_exact != right_exact) {
                return left_exact;
            }
            if (left.syllable_ids_len != right.syllable_ids_len) {
                return left.syllable_ids_len < right.syllable_ids_len;
            }
        }
        if (left.frequency != right.frequency) {
            return left.frequency > right.frequency;
        }
        const auto left_code = code_view(left_index);
        const auto right_code = code_view(right_index);
        if (left_code != right_code) {
            return left_code < right_code;
        }
        const auto left_text = text_view(left_index);
        const auto right_text = text_view(right_index);
        if (left_text.size() != right_text.size()) {
            return left_text.size() < right_text.size();
        }
        if (left_text != right_text) {
            return left_text < right_text;
        }
        return left_index < right_index;
    };

    std::vector<uint32_t> best_entries;
    best_entries.reserve(static_cast<size_t>(limit));
    uint32_t exact_count = 0;
    uint32_t prefix_count = 0;
    const bool use_prefix_index =
        wubi_prefix_index_ != nullptr && wubi_prefix_index_->is_loaded() && !code_prefix.empty();
    const bool filter_disabled = disabled_system_entry_count() != 0;
    if (use_prefix_index) {
        WubiPrefixMatch match;
        if (wubi_prefix_index_->find(code_prefix, &match)) {
            const std::size_t scan_limit =
                (std::min)(static_cast<std::size_t>(match.count),
                           static_cast<std::size_t>(limit) + kDisabledEntryOverfetch);
            for (std::size_t position = 0;
                 position < scan_limit && best_entries.size() < static_cast<std::size_t>(limit);
                 ++position) {
                ++prefix_count;
                const uint32_t index = match.entry_indexes[position];
                if (!filter_disabled) {
                    best_entries.push_back(index);
                    continue;
                }
                const auto& entry = dict_entries_[index];
                const std::string text(dict_strings_ + entry.text_offset, entry.text_len);
                if (!is_system_entry_disabled(text)) {
                    best_entries.push_back(index);
                }
            }
        }
    } else {
        uint32_t lower = 0;
        uint32_t upper = dict_entry_count_;
        while (lower < upper) {
            const uint32_t middle = lower + (upper - lower) / 2;
            const auto& entry = dict_entries_[middle];
            const char* code = dict_strings_ + entry.syllable_ids_offset;
            const uint32_t compare_length = (std::min)(entry.syllable_ids_len, prefix_length);
            const int comparison = std::memcmp(code, prefix_data, compare_length);
            if (comparison < 0 || (comparison == 0 && entry.syllable_ids_len < prefix_length)) {
                lower = middle + 1;
            } else {
                upper = middle;
            }
        }

        for (uint32_t index = lower; index < dict_entry_count_; ++index) {
            const auto& entry = dict_entries_[index];
            if (entry.syllable_ids_len < prefix_length ||
                std::memcmp(dict_strings_ + entry.syllable_ids_offset, prefix_data,
                            prefix_length) != 0) {
                break;
            }

            if (entry.syllable_ids_len == prefix_length) {
                ++exact_count;
            } else {
                ++prefix_count;
            }

            if (filter_disabled) {
                const std::string text(dict_strings_ + entry.text_offset, entry.text_len);
                if (is_system_entry_disabled(text)) {
                    continue;
                }
            }

            bool duplicate = false;
            for (auto& selected_index : best_entries) {
                if (text_view(selected_index) == text_view(index)) {
                    if (entry_better(index, selected_index)) {
                        selected_index = index;
                    }
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }

            if (best_entries.size() < static_cast<size_t>(limit)) {
                best_entries.push_back(index);
                continue;
            }

            size_t worst = 0;
            for (size_t candidate = 1; candidate < best_entries.size(); ++candidate) {
                if (entry_better(best_entries[worst], best_entries[candidate])) {
                    worst = candidate;
                }
            }
            if (entry_better(index, best_entries[worst])) {
                best_entries[worst] = index;
            }
        }
        std::sort(best_entries.begin(), best_entries.end(), entry_better);
    }

    results.reserve(best_entries.size() + static_cast<size_t>(limit));
    // Preserve the offline index order while keeping learned exact entries dominant.
    static constexpr int kSystemRankBase = 100000;
    for (size_t rank = 0; rank < best_entries.size(); ++rank) {
        const uint32_t index = best_entries[rank];
        const auto& entry = dict_entries_[index];
        Candidate candidate;
        fill_system_candidate(index, candidate, 0);
        candidate.source_frequency = entry.frequency;
        if (use_prefix_index) {
            candidate.frequency = kSystemRankBase - static_cast<int>(rank);
        } else {
            candidate.frequency = (entry.syllable_ids_len == prefix_length ? 100000 : 0) +
                                  (100 - static_cast<int>(entry.syllable_ids_len)) * 100 +
                                  entry.frequency;
        }
        results.push_back(std::move(candidate));
    }

    // The user's word lists: after the system words of the same kind of match (the exact code,
    // then each longer code), so that the positions a Wubi typist knows stay where they were.
    if (const auto extension = std::atomic_load(&extension_)) {
        const auto group = [&](const std::string& code) {
            return std::make_pair(code.size() == prefix_length ? 0 : 1, code.size());
        };
        bool added = false;
        for (const ExtensionLexicon::Word* word :
             extension->match_code_prefix(code_prefix, static_cast<std::size_t>(limit))) {
            if (filter_disabled && is_system_entry_disabled(word->text)) continue;
            if (std::any_of(results.begin(), results.end(),
                            [&](const Candidate& c) { return c.text == word->text; })) {
                continue;
            }
            Candidate candidate;
            candidate.text = word->text;
            candidate.code = word->code;
            const auto at = std::find_if(results.begin(), results.end(), [&](const Candidate& c) {
                return group(c.code) > group(candidate.code);
            });
            results.insert(at, std::move(candidate));
            added = true;
        }
        if (added && use_prefix_index) {
            for (size_t rank = 0; rank < results.size(); ++rank) {
                results[rank].frequency = kSystemRankBase - static_cast<int>(rank);
            }
        }
    }

    QueryBudget user_budget;
    UserLookupStats user_stats;
    auto user_results = lookup_user_prefix(code_prefix, limit, user_budget, trace, &user_stats);
    for (auto& candidate : user_results) {
        merge_candidate(results, std::move(candidate), prefix_length, use_prefix_index);
    }

    std::sort(results.begin(), results.end(), [&](const Candidate& left, const Candidate& right) {
        return candidate_better(left, right, prefix_length, use_prefix_index);
    });
    if (results.size() > static_cast<size_t>(limit)) {
        results.resize(static_cast<size_t>(limit));
    }

    if (trace) {
        trace->exact_scan_count += exact_count;
        trace->prefix_scan_count += prefix_count;
    }
    return results;
}

std::vector<Candidate> Dict::lookup(const std::string& code_prefix, int limit,
                                    const QueryBudget& budget, QueryTrace* trace) {
    if (budget.deadline.enabled && budget.deadline.expired()) {
        if (trace) {
            trace->deadline_exceeded = true;
            trace->truncated = true;
        }
        return {};
    }
    return lookup(code_prefix, limit, trace);
}

} // namespace cxxime
