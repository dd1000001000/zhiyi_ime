// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/dict.h>

#include <algorithm>
#include <climits>

#include <cxxime/query_budget.h>

namespace cxxime {

namespace {

bool candidate_better(const Candidate& left, const Candidate& right) {
    if (left.frequency != right.frequency) {
        return left.frequency > right.frequency;
    }
    return left.text < right.text;
}

void offer_bounded(std::vector<Candidate>& candidates, Candidate candidate, size_t capacity) {
    for (auto& existing : candidates) {
        if (existing.text == candidate.text) {
            if (candidate_better(candidate, existing)) {
                existing = std::move(candidate);
            }
            return;
        }
    }

    if (capacity == 0) {
        return;
    }
    if (candidates.size() < capacity) {
        candidates.push_back(std::move(candidate));
        return;
    }

    size_t worst = 0;
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (candidate_better(candidates[worst], candidates[i])) {
            worst = i;
        }
    }
    if (candidate_better(candidate, candidates[worst])) {
        candidates[worst] = std::move(candidate);
    }
}

} // namespace

bool Dict::lookup_exact_span(const std::vector<uint32_t>& ids, size_t start, size_t end,
                             const SpanLookupLimits& limits, const QueryDeadline& deadline,
                             std::vector<Candidate>& output, SpanLookupStats& stats) const {
    output.clear();
    if (start >= end || end > ids.size() || id_index_.empty()) {
        return false;
    }
    if (stats.range_queries >= limits.max_range_queries ||
        stats.entry_scans >= limits.max_entry_scans || stats.result_count >= limits.max_results) {
        stats.truncated = true;
        return false;
    }
    if (deadline.enabled && deadline.expired()) {
        stats.deadline_exceeded = true;
        stats.truncated = true;
        return false;
    }

    ++stats.range_queries;
    const uint32_t* query = ids.data() + start;
    const size_t query_size = end - start;
    auto entry_less = [&](const IdEntry& entry) {
        const size_t common = (std::min)(static_cast<size_t>(entry.count), query_size);
        for (size_t i = 0; i < common; ++i) {
            if (entry.ids[i] < query[i]) {
                return true;
            }
            if (entry.ids[i] > query[i]) {
                return false;
            }
        }
        return static_cast<size_t>(entry.count) < query_size;
    };

    size_t low = 0;
    size_t high = id_index_.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (entry_less(id_index_[middle])) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= id_index_.size()) {
        return false;
    }

    auto prefix_matches = [&](const IdEntry& entry) {
        if (static_cast<size_t>(entry.count) < query_size) {
            return false;
        }
        return std::equal(query, query + query_size, entry.ids);
    };
    if (!prefix_matches(id_index_[low])) {
        return false;
    }

    auto exact_matches = [&](const IdEntry& entry) {
        return static_cast<size_t>(entry.count) == query_size &&
               std::equal(query, query + query_size, entry.ids);
    };

    std::vector<Candidate> candidates;
    candidates.reserve(limits.max_candidates_per_range);
    // Entries with the same syllables are stored by frequency, highest first (dict.bin is built
    // ORDER BY syllable_ids, frequency DESC and the index keeps that order). Once the bounded list
    // is full, the rest cannot enter it: stop instead of scanning every homophone (several hundred
    // for "yi" or "shi"), which spent the scan budget of a long sentence before its last syllable.
    bool descending = true;
    int previous_frequency = INT_MAX;
    for (size_t position = low; position < id_index_.size() && exact_matches(id_index_[position]);
         ++position) {
        if (stats.entry_scans >= limits.max_entry_scans) {
            stats.truncated = true;
            break;
        }
        if (deadline.enabled && deadline.check_interval > 0 && stats.entry_scans > 0 &&
            stats.entry_scans % deadline.check_interval == 0 && deadline.expired()) {
            stats.deadline_exceeded = true;
            stats.truncated = true;
            break;
        }

        ++stats.entry_scans;
        Candidate candidate;
        fill_system_candidate(id_index_[position].index, candidate, 0);
        candidate.source_frequency = candidate.frequency;
        const int frequency = candidate.frequency;
        descending = descending && frequency <= previous_frequency;
        previous_frequency = frequency;
        offer_bounded(candidates, std::move(candidate), limits.max_candidates_per_range);
        if (descending && candidates.size() >= limits.max_candidates_per_range) {
            break;
        }
    }

    std::sort(candidates.begin(), candidates.end(), candidate_better);
    const uint32_t remaining = limits.max_results - stats.result_count;
    if (candidates.size() > remaining) {
        candidates.resize(remaining);
        stats.truncated = true;
    }
    stats.result_count += static_cast<uint32_t>(candidates.size());
    output = std::move(candidates);
    return true;
}

void Dict::lookup_exact_spans(const std::vector<uint32_t>& ids, size_t start,
                              const SpanLookupLimits& limits, const QueryDeadline& deadline,
                              std::vector<SpanCandidate>& output, SpanLookupStats& stats) const {
    output.clear();
    if (start >= ids.size()) {
        return;
    }

    std::vector<Candidate> candidates;
    for (size_t end = start + 1; end <= ids.size(); ++end) {
        const bool has_prefix =
            lookup_exact_span(ids, start, end, limits, deadline, candidates, stats);
        if (!has_prefix) {
            break;
        }
        for (auto& candidate : candidates) {
            SpanCandidate span;
            span.end = static_cast<uint16_t>(end);
            span.candidate = std::move(candidate);
            output.push_back(std::move(span));
        }
        if (stats.truncated) {
            break;
        }
    }
}

} // namespace cxxime
