// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/translator.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <iterator>
#include <utility>

#include <cxxime/composition_learning.h>
#include <cxxime/pinyin_composer.h>
#include <cxxime/pinyin_resource.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_scratch.h>
#include <cxxime/query_trace.h>
#include <cxxime/short_code_cache.h>
#include <cxxime/topk_collector.h>

#include "pinyin_path_filter.h"
#include "pinyin_partial_candidates.h"
#include "pinyin_query_key.h"

namespace cxxime {

namespace {

constexpr int kDisabledTopnOverfetch = 16;
// Keep fallback tiers aligned with scripts/build_pinyin_topn.py.
constexpr int kExactCompleteBase = 100000000;
// Fuzzy pinyin treats both spellings of a pair as the same sound (xin = xing): fuzzy matches
// share the exact tiers and frequency decides (fuzzy is only searched when the user enabled it).
constexpr int kFuzzyCompleteBase = kExactCompleteBase;
constexpr int kExactPrefixBase = 80000000;
constexpr int kAbbreviationCompleteBase = 60000000;
constexpr int kMixedCompleteBase = 50000000;
constexpr int kFuzzyPrefixBase = kExactPrefixBase;
constexpr int kAbbreviationPrefixBase = 30000000;
constexpr int kMixedPrefixBase = 20000000;
constexpr int kMaxRankedSourceFrequency = 99999900;

enum class PathMatchTier {
    kNormal,
    kFuzzy,
    kAbbreviation,
    kMixed,
    kCompletion,
};

PathMatchTier classify_path(const SegmentedPath& path) {
    bool has_normal_or_fuzzy = false;
    bool has_fuzzy = false;
    bool has_abbreviation = false;
    bool has_completion = false;
    for (uint8_t type : path.spelling_types) {
        has_normal_or_fuzzy = has_normal_or_fuzzy || type <= kFuzzySpelling;
        has_fuzzy = has_fuzzy || type == kFuzzySpelling;
        has_abbreviation = has_abbreviation || type == kAbbreviation;
        has_completion = has_completion || type == kCompletionSpelling;
    }
    if (has_completion) {
        return PathMatchTier::kCompletion;
    }
    if (has_abbreviation) {
        return has_normal_or_fuzzy ? PathMatchTier::kMixed : PathMatchTier::kAbbreviation;
    }
    return has_fuzzy ? PathMatchTier::kFuzzy : PathMatchTier::kNormal;
}

std::size_t candidate_syllable_count(const Candidate& candidate) {
    if (candidate.syllables.empty()) {
        return 0;
    }
    return 1 + static_cast<std::size_t>(
                   std::count(candidate.syllables.begin(), candidate.syllables.end(), ':'));
}

void rank_fallback_candidate(Candidate& candidate, PathMatchTier tier,
                             std::size_t query_syllable_count) {
    const bool complete = candidate_syllable_count(candidate) == query_syllable_count;
    int base = 0;
    switch (tier) {
        case PathMatchTier::kNormal:
            base = complete ? kExactCompleteBase : kExactPrefixBase;
            break;
        case PathMatchTier::kFuzzy:
        case PathMatchTier::kCompletion:
            base = complete ? kFuzzyCompleteBase : kFuzzyPrefixBase;
            break;
        case PathMatchTier::kAbbreviation:
            base = complete ? kAbbreviationCompleteBase : kAbbreviationPrefixBase;
            break;
        case PathMatchTier::kMixed:
            base = complete ? kMixedCompleteBase : kMixedPrefixBase;
            break;
    }
    const int source_frequency = (std::max)(0, candidate.source_frequency);
    candidate.frequency = base + (std::min)(source_frequency, kMaxRankedSourceFrequency) / 100;
}

struct CompositionPathSpec {
    size_t id_sequence_index = 0;
    size_t segmented_path_index = 0;
    CompositionPathKind kind = CompositionPathKind::kNormal;
    uint16_t rank = 0;
};

struct IndexedShuangpinPath {
    std::string key;
    PathMatchTier tier = PathMatchTier::kNormal;
    std::size_t syllable_count = 0;
};

} // namespace

// Linear dedup helpers — cheaper than hash set for small collections (≤128)
static bool contains_text(const std::vector<Candidate>& items, const std::string& text) {
    for (auto& c : items)
        if (c.text == text) return true;
    return false;
}

static void merge_candidate_by_score(std::vector<Candidate>& items, Candidate candidate) {
    for (auto& item : items) {
        if (item.text == candidate.text) {
            if (candidate.frequency > item.frequency)
                item = std::move(candidate);
            return;
        }
    }
    items.push_back(std::move(candidate));
}

static void sort_candidates_by_score(std::vector<Candidate>& items) {
    std::sort(items.begin(), items.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.frequency != b.frequency) return a.frequency > b.frequency;
            if (a.text.size() != b.text.size()) return a.text.size() < b.text.size();
            return a.text < b.text;
        });
}

static void remove_oversized_candidates(std::vector<Candidate>& candidates) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](const Candidate& candidate) {
                                        return !candidate_text_fits(candidate.text);
                                    }),
                     candidates.end());
}

static void remove_repeated_short_extensions(std::vector<Candidate>& candidates,
                                             const std::vector<Candidate>& composed) {
    candidates.erase(
        std::remove_if(
            candidates.begin(), candidates.end(),
            [&](const Candidate& candidate) {
                return std::any_of(composed.begin(), composed.end(), [&](const Candidate& exact) {
                    return candidate.text.size() > exact.text.size() &&
                           candidate.text.compare(0, exact.text.size(), exact.text) == 0;
                });
            }),
        candidates.end());
}

static bool contains_ids(const std::vector<std::vector<uint32_t>>& items,
                         const std::vector<uint32_t>& ids) {
    for (auto& v : items)
        if (v == ids) return true;
    return false;
}

void PinyinTranslator::set_dict(Dict* dict) {
    dict_ = dict;
    query_cache_.clear();
    query_cache_sequence_ = 0;
}

void PinyinTranslator::bind_pinyin(std::shared_ptr<const PinyinResourceSet> resources,
                                   PinyinQueryPolicy policy) {
    if (pinyin_resources_ == resources &&
        pinyin_query_policy_.enable_fuzzy == policy.enable_fuzzy &&
        pinyin_query_policy_.fuzzy_groups == policy.fuzzy_groups &&
        pinyin_query_policy_.initials_only == policy.initials_only) {
        return;
    }
    pinyin_resources_ = std::move(resources);
    pinyin_query_policy_ = policy;
    query_cache_.clear();
}

PinyinSchemeKind PinyinTranslator::pinyin_scheme() const {
    return pinyin_resources_ ? pinyin_resources_->kind() : PinyinSchemeKind::kFullPinyin;
}

void PinyinTranslator::set_sentence_composition_enabled(bool enabled) {
    if (sentence_composition_enabled_ == enabled) {
        return;
    }
    sentence_composition_enabled_ = enabled;
    query_cache_.clear();
}

void PinyinTranslator::set_candidate_learning_enabled(bool enabled) {
    if (candidate_learning_enabled_ == enabled) {
        return;
    }
    candidate_learning_enabled_ = enabled;
    query_cache_.clear();
}

void PinyinTranslator::set_composition_learning_service(CompositionLearningService* service) {
    if (composition_learning_ == service) {
        return;
    }
    composition_learning_ = service;
    query_cache_.clear();
}

bool PinyinTranslator::is_indexable_key(const std::string& pinyin) {
    if (pinyin.empty())
        return false;
    for (char c : pinyin) {
        if (c < 'a' || c > 'z')
            return false;
    }
    return true;
}

PinyinTranslator::IndexedFastResult PinyinTranslator::lookup_indexed_fast(
    const std::string& key, int limit, QueryTrace* trace) const {
    IndexedFastResult result;
    if (limit <= 0)
        return result;

    // 1. User dictionary indexes
    if (dict_) {
        QueryBudget ub;
        ub.max_user_scan = 64;  // tight budget for fast path
        UserLookupStats ustats;
        auto user_results = dict_->lookup_user_indexed(key, limit, ub, trace, &ustats);
        for (auto& c : user_results) {
            merge_candidate_by_score(result.candidates, std::move(c));
        }
    }

    // 2. Pre-built Top-N index
    if (short_cache_ && short_cache_->is_loaded()) {
        bool prefix_complete = false;
        const bool filter_disabled = dict_ && dict_->disabled_system_entry_count() != 0;
        const int cache_limit = filter_disabled && limit <= INT_MAX - kDisabledTopnOverfetch
                                    ? limit + kDisabledTopnOverfetch
                                    : limit;
        auto cached = short_cache_->lookup(key, cache_limit, trace, &prefix_complete);
        const bool posting_exhausted = cached.size() < static_cast<std::size_t>(cache_limit);
        if (filter_disabled) {
            dict_->filter_disabled_system_candidates(cached);
        }
        result.complete_index_hit =
            prefix_complete &&
            (posting_exhausted || cached.size() >= static_cast<std::size_t>(limit));
        for (auto& c : cached) {
            merge_candidate_by_score(result.candidates, std::move(c));
        }
    }

    if (candidate_learning_enabled_ && dict_) {
        dict_->apply_candidate_preferences(key, CandidateSource::kPinyin, result.candidates,
                                           limit);
    }
    if (candidate_learning_enabled_ && composition_learning_) {
        auto learned = composition_learning_->lookup_candidates(key, limit);
        if (dict_) {
            dict_->filter_disabled_system_candidates(learned);
        }
        for (auto& candidate : learned) {
            merge_candidate_by_score(result.candidates, std::move(candidate));
        }
    }
    sort_candidates_by_score(result.candidates);
    if ((int)result.candidates.size() > limit)
        result.candidates.resize(limit);
    if (dict_) {
        dict_->apply_manual_candidate_order(key, CandidateSource::kPinyin, result.candidates,
                                            limit);
    }
    result.hit = !result.candidates.empty();
    return result;
}

PinyinTranslator::QueryCacheVersions PinyinTranslator::query_cache_versions() const {
    QueryCacheVersions versions;
    if (dict_) {
        versions.user_dict = dict_->user_dict_version();
        versions.candidate_preference =
            candidate_learning_enabled_ ? dict_->candidate_preference_version() : 0;
        versions.manual_candidate_order = dict_->manual_candidate_order_version();
        versions.disabled_system_entry = dict_->disabled_system_entry_version();
    }
    versions.composition_learning = candidate_learning_enabled_ && composition_learning_
                                        ? composition_learning_->version()
                                        : 0;
    return versions;
}

bool PinyinTranslator::lookup_query_cache(const std::string& input, int page_index,
                                          int candidate_offset, int page_size,
                                          const QueryCacheVersions& versions,
                                          CandidatePage& page, QueryTrace* trace) {
    if (!dict_)
        return false;

    for (auto& entry : query_cache_) {
        if (entry.input == input &&
            entry.page_index == page_index &&
            entry.candidate_offset == candidate_offset &&
            entry.page_size == page_size &&
            entry.user_dict_version == versions.user_dict &&
            entry.candidate_preference_version == versions.candidate_preference &&
            entry.manual_candidate_order_version == versions.manual_candidate_order &&
            entry.disabled_system_entry_version == versions.disabled_system_entry &&
            entry.composition_learning_version == versions.composition_learning) {
            entry.sequence = ++query_cache_sequence_;
            page = entry.page;
            if (trace) {
                trace->cache_hit = true;
                trace->exact_scan_count = 0;
                trace->prefix_scan_count = 0;
                trace->user_scan_count = 0;
                trace->syllable_path_count = 0;
                trace->live_path_count = 0;
                trace->composition_path_count = 0;
                trace->composition_repeated_short_path_count = 0;
                trace->span_query_count = 0;
                trace->span_entry_scan_count = 0;
                trace->composition_state_count = 0;
                trace->composed_candidate_count = 0;
                trace->composition_truncated = false;
                trace->composition_us = 0;
                trace->deadline_exceeded = false;
            }
            return true;
        }
    }
    return false;
}

void PinyinTranslator::store_query_cache(const std::string& input, int page_index,
                                         int candidate_offset, int page_size,
                                         const QueryCacheVersions& versions,
                                         const CandidatePage& page) {
    if (!dict_)
        return;

    const QueryCacheVersions current_versions = query_cache_versions();
    if (current_versions.user_dict != versions.user_dict ||
        current_versions.candidate_preference != versions.candidate_preference ||
        current_versions.manual_candidate_order != versions.manual_candidate_order ||
        current_versions.disabled_system_entry != versions.disabled_system_entry ||
        current_versions.composition_learning != versions.composition_learning) {
        return;
    }
    for (auto& entry : query_cache_) {
        if (entry.input == input &&
            entry.page_index == page_index &&
            entry.candidate_offset == candidate_offset &&
            entry.page_size == page_size &&
            entry.user_dict_version == versions.user_dict &&
            entry.candidate_preference_version == versions.candidate_preference &&
            entry.manual_candidate_order_version == versions.manual_candidate_order &&
            entry.disabled_system_entry_version == versions.disabled_system_entry &&
            entry.composition_learning_version == versions.composition_learning) {
            entry.page = page;
            entry.sequence = ++query_cache_sequence_;
            return;
        }
    }

    if (query_cache_.size() >= kMaxQueryCacheEntries) {
        size_t oldest = 0;
        for (size_t i = 1; i < query_cache_.size(); ++i) {
            if (query_cache_[i].sequence < query_cache_[oldest].sequence)
                oldest = i;
        }
        query_cache_.erase(query_cache_.begin() + oldest);
    }

    QueryCacheEntry entry;
    entry.input = input;
    entry.page_index = page_index;
    entry.candidate_offset = candidate_offset;
    entry.page_size = page_size;
    entry.user_dict_version = versions.user_dict;
    entry.candidate_preference_version = versions.candidate_preference;
    entry.manual_candidate_order_version = versions.manual_candidate_order;
    entry.disabled_system_entry_version = versions.disabled_system_entry;
    entry.composition_learning_version = versions.composition_learning;
    entry.sequence = ++query_cache_sequence_;
    entry.page = page;
    query_cache_.push_back(std::move(entry));
}

// Fuzzy pinyin: a candidate that the typed letters reach only through a fuzzy spelling
// (zong -> 中) shows its correct pinyin after the text: "中 (zhong)".
SyllabifierOptions PinyinTranslator::fuzzy_options() const {
    SyllabifierOptions options;
    options.enable_fuzzy = pinyin_query_policy_.enable_fuzzy;
    options.fuzzy_groups = pinyin_query_policy_.fuzzy_groups;
    return options;
}

void PinyinTranslator::annotate_fuzzy_matches(const std::string& input,
                                              std::vector<CandidateEntry>& entries, int begin,
                                              int end) const {
    if (!pinyin_query_policy_.enable_fuzzy || !pinyin_resources_ ||
        pinyin_scheme() != PinyinSchemeKind::kFullPinyin) {
        return;
    }
    for (int index = begin; index < end; ++index) {
        Candidate& candidate = entries[index].candidate;
        const auto* action = std::get_if<TextSelectionAction>(&entries[index].selection);
        if (candidate.syllables.empty() || !entries[index].hint.empty() || !action ||
            action->consumed_input_bytes == 0 || action->consumed_input_bytes > input.size()) {
            continue;
        }
        const std::string typed = input.substr(0, action->consumed_input_bytes);
        if (!pinyin_resources_->matches_without_fuzzy(typed, candidate.syllables)) {
            // The entry hint is what the candidate window shows after the text.
            std::string pinyin = candidate.syllables;
            std::replace(pinyin.begin(), pinyin.end(), ':', ' ');
            entries[index].hint = pinyin;
            candidate.comment = std::move(pinyin);
        }
    }
}

void PinyinTranslator::keep_initials_matches(const std::string& pinyin,
                                             std::vector<Candidate>& candidates) const {
    if (!pinyin_query_policy_.initials_only) {
        return;
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const Candidate& candidate) {
                                        return !pinyin_matches_initials(pinyin,
                                                                        candidate.syllables);
                                    }),
                     candidates.end());
}

CandidatePage PinyinTranslator::translate_page(const std::string& pinyin, int page_index,
                                               int page_size, QueryTrace* trace,
                                               const QueryBudget* budget, QueryScratch* scratch,
                                               int candidate_offset,
                                               bool require_runtime_paths) {
    CandidatePage page;
    page.page_index = page_index;
    page.page_size = page_size;

    if (!dict_ || !dict_->is_open() || pinyin.empty())
        return page;

    int offset = candidate_offset >= 0 ? candidate_offset : page_index * page_size;
    page.page_offset = offset;
    int fetch_limit = page_size;
    const int need = offset + fetch_limit + 1;
    const QueryCacheVersions cache_versions = query_cache_versions();

    if (!require_runtime_paths &&
        lookup_query_cache(pinyin, page_index, offset, page_size, cache_versions, page, trace))
        return page;

    IndexedFastResult fast;
    auto finish_complete_fast_path = [&]() {
        if (require_runtime_paths || !fast.complete_index_hit ||
            (sentence_composition_enabled_ && static_cast<int>(fast.candidates.size()) < need)) {
            return false;
        }
        auto& sorted = fast.candidates;
        const int known_count = static_cast<int>(sorted.size());
        const int returned_end = (std::min)(offset + fetch_limit, known_count);
        page.extent = make_candidate_extent(known_count, returned_end, false);
        if (offset > 0 && offset < static_cast<int>(sorted.size())) {
            sorted.erase(sorted.begin(), sorted.begin() + offset);
        }
        if (static_cast<int>(sorted.size()) > fetch_limit) {
            sorted.resize(fetch_limit);
            if (trace) {
                trace->truncated = true;
                trace->page_truncated = true;
            }
        }
        page.candidates = std::move(sorted);
        for (auto& candidate : page.candidates) {
            candidate.source = CandidateSource::kPinyin;
        }
        if (!page.candidates.empty()) {
            page.highlighted = 0;
        }
        if (trace) {
            trace->cache_hit = true;
            trace->exact_scan_count = 0;
            trace->prefix_scan_count = 0;
            trace->deadline_exceeded = false;
        }
        return true;
    };

    // Full Pinyin keeps its existing zero-decode Top-N fast path.
    if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin && is_indexable_key(pinyin)) {
        fast = lookup_indexed_fast(pinyin, need, trace);
        remove_oversized_candidates(fast.candidates);
        keep_initials_matches(pinyin, fast.candidates);
        if (finish_complete_fast_path()) {
            return page;
        }
    }

    // Collect syllable ID sequences to try (use scratch if available)
    QueryScratch local_scratch;
    QueryScratch& scr = scratch ? *scratch : local_scratch;
    auto& id_sequences = scr.id_sequences;
    std::vector<PathMatchTier> path_tiers;
    std::vector<std::string> path_query_keys;

    auto add_path = [&](const std::vector<std::string>& syllables,
                        PathMatchTier tier, std::string query_key = {}) -> size_t {
        if (syllables.empty()) return SIZE_MAX;
        std::vector<uint32_t> ids;
        for (auto& s : syllables) {
            uint32_t id = dict_->syllable_to_id(s);
            if (id == UINT32_MAX) return SIZE_MAX;
            ids.push_back(id);
        }
        id_sequences.push_back(std::move(ids));
        path_tiers.push_back(tier);
        path_query_keys.push_back(std::move(query_key));
        return id_sequences.size() - 1;
    };

    // 1. Syllabifier for abbreviation expansion (reserve first)
    // Limit paths to avoid CPU cache thrashing on short inputs (e.g. single letter 's')
    // The syllabifier checks the deadline internally, so it does not need to be skipped.
    // Need enough paths for fuzzy spellings — abbreviation-heavy graphs can
    // produce hundreds of paths before non-abbreviation paths appear.
    static constexpr size_t kMaxPaths = 64;
    static constexpr size_t kMaxCompositionPaths = 8;
    static constexpr size_t kMaxIndexedShuangpinPaths = 8;
    auto rank_shuangpin_path = [&](const std::string& query_key,
                                   std::vector<Candidate>& candidates) {
        if (query_key.empty()) {
            return;
        }
        if (candidate_learning_enabled_) {
            dict_->apply_candidate_preferences(query_key, CandidateSource::kPinyin, candidates,
                                               need);
        }
        dict_->filter_disabled_system_candidates(candidates);
        if (candidate_learning_enabled_ && composition_learning_) {
            auto learned = composition_learning_->lookup_candidates(query_key, need);
            dict_->filter_disabled_system_candidates(learned);
            for (auto& candidate : learned) {
                candidate.input_code = query_key;
                merge_candidate_by_score(candidates, std::move(candidate));
            }
            sort_candidates_by_score(candidates);
            if (static_cast<int>(candidates.size()) > need) {
                candidates.resize(need);
            }
        }
        for (auto& candidate : candidates) {
            candidate.input_code = query_key;
        }
    };
    bool deadline_hit = false;
    SegmentResult segment_result;
    std::vector<CompositionPathSpec> composition_specs;
    if (sentence_composition_enabled_) {
        composition_specs.reserve(kMaxCompositionPaths + 1);
    }
    if (pinyin_resources_) {
        // Check deadline before syllabifier (it can be slow on long inputs)
        if (budget && budget->deadline.expired()) {
            deadline_hit = true;
        } else {
            // Pass the deadline to the syllabifier for internal checks.
            SyllabifierOptions options;
            options.enable_fuzzy = pinyin_query_policy_.enable_fuzzy;
            options.fuzzy_groups = pinyin_query_policy_.fuzzy_groups;
            options.enable_terminal_completion =
                pinyin_scheme() == PinyinSchemeKind::kShuangpin;
            options.collect_path_metadata = true;
            segment_result = pinyin_resources_->segment(
                pinyin, budget ? &budget->deadline : nullptr, options);
            id_sequences.reserve(std::min(segment_result.paths.size(), kMaxPaths) + 1);
            path_tiers.reserve(std::min(segment_result.paths.size(), kMaxPaths) + 1);
            path_query_keys.reserve(std::min(segment_result.paths.size(), kMaxPaths) + 1);
            bool has_normal_composition_path = false;
            bool has_repeated_short_path = false;
            CompositionPathSpec repeated_short_spec;
            for (size_t i = 0; i < segment_result.paths.size() && i < kMaxPaths; ++i) {
                const auto& segmented_path = segment_result.paths[i];
                if (pinyin_scheme() == PinyinSchemeKind::kShuangpin &&
                    !path_consumes_entire_input(pinyin, segmented_path)) {
                    continue;
                }
                const size_t id_index = add_path(
                    segmented_path.syllables, classify_path(segmented_path),
                    pinyin_scheme() == PinyinSchemeKind::kShuangpin
                        ? canonical_pinyin_key(segmented_path.syllables)
                        : std::string{});
                if (id_index == SIZE_MAX) {
                    continue;
                }

                if (sentence_composition_enabled_) {
                    const bool duplicate_composition_path = std::any_of(
                        composition_specs.begin(), composition_specs.end(),
                        [&](const auto& spec) {
                            return id_sequences[spec.id_sequence_index] ==
                                   id_sequences[id_index];
                        });
                    const bool normal_composition_path =
                        pinyin_scheme() == PinyinSchemeKind::kShuangpin
                            ? is_complete_normal_path(pinyin, segmented_path)
                            : is_normal_composition_path(pinyin, segmented_path);
                    if (normal_composition_path) {
                        has_normal_composition_path = true;
                        if (!duplicate_composition_path &&
                            composition_specs.size() < kMaxCompositionPaths) {
                            composition_specs.push_back({id_index, i,
                                                        CompositionPathKind::kNormal,
                                                        static_cast<uint16_t>(i)});
                        }
                    } else if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin &&
                               !has_repeated_short_path &&
                               is_repeated_short_code_path(pinyin, segmented_path)) {
                        repeated_short_spec = {id_index, i,
                                               CompositionPathKind::kRepeatedShortCode,
                                               static_cast<uint16_t>(i)};
                        has_repeated_short_path = true;
                    }
                }
            }
            if (!has_normal_composition_path && has_repeated_short_path) {
                composition_specs.push_back(repeated_short_spec);
            }
            if (segment_result.deadline_exceeded) {
                deadline_hit = true;
                if (trace) {
                    trace->deadline_exceeded = true;
                    trace->truncated = true;
                }
            }
        }
    } else {
        id_sequences.reserve(2);
    }

    if (pinyin_scheme() == PinyinSchemeKind::kShuangpin && !deadline_hit) {
        std::vector<IndexedShuangpinPath> indexed_paths;
        for (std::size_t index = 0;
             index < segment_result.paths.size() && index < kMaxPaths; ++index) {
            const auto& path = segment_result.paths[index];
            if (!path_consumes_entire_input(pinyin, path)) {
                continue;
            }
            const std::string key = canonical_pinyin_key(path.syllables);
            if (key.empty() || !is_indexable_key(key)) {
                continue;
            }
            const PathMatchTier tier = classify_path(path);
            const auto existing = std::find_if(
                indexed_paths.begin(), indexed_paths.end(),
                [&](const auto& item) { return item.key == key; });
            if (existing != indexed_paths.end()) {
                if (static_cast<int>(tier) < static_cast<int>(existing->tier)) {
                    existing->tier = tier;
                }
                continue;
            }
            indexed_paths.push_back({key, tier, path.syllables.size()});
            if (indexed_paths.size() >= kMaxIndexedShuangpinPaths) {
                break;
            }
        }
        for (const auto& path : indexed_paths) {
            IndexedFastResult path_fast = lookup_indexed_fast(path.key, need, trace);
            for (auto& candidate : path_fast.candidates) {
                candidate.input_code = path.key;
                if (path.tier != PathMatchTier::kNormal) {
                    rank_fallback_candidate(candidate, path.tier, path.syllable_count);
                }
            }
            rank_shuangpin_path(path.key, path_fast.candidates);
            for (auto& candidate : path_fast.candidates) {
                merge_candidate_by_score(fast.candidates, std::move(candidate));
            }
            fast.hit = fast.hit || path_fast.hit;
            fast.complete_index_hit = indexed_paths.size() == 1 &&
                                      path.tier == PathMatchTier::kNormal &&
                                      path_fast.complete_index_hit;
        }
        if (indexed_paths.size() != 1) {
            fast.complete_index_hit = false;
        }
        remove_oversized_candidates(fast.candidates);
        sort_candidates_by_score(fast.candidates);
        if (static_cast<int>(fast.candidates.size()) > need) {
            fast.candidates.resize(need);
        }
        for (auto it = indexed_paths.rbegin(); it != indexed_paths.rend(); ++it) {
            dict_->apply_manual_candidate_order(it->key, CandidateSource::kPinyin,
                                                fast.candidates, need, it->key);
        }
        if (finish_complete_fast_path()) {
            return page;
        }
    }

    // 2. Normal segmentation (skip if deadline already hit)
    if (!deadline_hit && pinyin_scheme() == PinyinSchemeKind::kFullPinyin)
        add_path(segmentor_.segment_best(pinyin), PathMatchTier::kNormal);

    // If deadline hit, return empty page with trace flags
    if (deadline_hit) {
        if (trace) {
            trace->deadline_exceeded = true;
            trace->truncated = true;
        }
        page.extent.state = CandidateExtentState::kIndeterminate;
        page.extent.complete = false;
        return page;
    }

    // Filter: only keep paths that actually have dict entries
    auto& live_path_indices = scr.live_path_indices;
    live_path_indices.reserve(id_sequences.size());
    auto collect_live_paths = [&](size_t first_path) {
        for (size_t i = first_path; i < id_sequences.size(); ++i) {
            // Check deadline before each has_prefix (syllabifier may have consumed most of the budget)
            if (budget && budget->deadline.expired()) {
                deadline_hit = true;
                if (trace) {
                    trace->deadline_exceeded = true;
                    trace->truncated = true;
                }
                break;
            }
            if (dict_->has_prefix(id_sequences[i], trace))
                live_path_indices.push_back(i);
        }
    };
    collect_live_paths(0);

    // If valid full-syllable paths have no dictionary continuation, retry with
    // terminal syllable completion (for example, "ji" -> "jie").
    if (live_path_indices.empty() && pinyin_resources_ && !deadline_hit &&
        !pinyin_query_policy_.initials_only) {
        SyllabifierOptions completion_options;
        completion_options.enable_fuzzy = pinyin_query_policy_.enable_fuzzy;
        completion_options.fuzzy_groups = pinyin_query_policy_.fuzzy_groups;
        completion_options.enable_terminal_completion = true;
        completion_options.collect_path_metadata = true;
        auto completion_result = pinyin_resources_->segment(
            pinyin, budget ? &budget->deadline : nullptr, completion_options);
        if (completion_result.deadline_exceeded) {
            deadline_hit = true;
            if (trace) {
                trace->deadline_exceeded = true;
                trace->truncated = true;
            }
        } else {
            const size_t first_completion = id_sequences.size();
            id_sequences.reserve(first_completion +
                std::min(completion_result.paths.size(), kMaxPaths));
            for (size_t i = 0;
                 i < completion_result.paths.size() && i < kMaxPaths; ++i) {
                const auto& completion_path = completion_result.paths[i];
                if (pinyin_scheme() == PinyinSchemeKind::kShuangpin &&
                    !path_consumes_entire_input(pinyin, completion_path)) {
                    continue;
                }
                add_path(completion_path.syllables, classify_path(completion_path),
                         pinyin_scheme() == PinyinSchemeKind::kShuangpin
                             ? canonical_pinyin_key(completion_path.syllables)
                             : std::string{});
            }
            collect_live_paths(first_completion);
        }
    }

    // Record path counts after the optional completion fallback.
    if (trace)
        trace->syllable_path_count = (int)id_sequences.size();

    // Record live path count
    if (trace)
        trace->live_path_count = (int)live_path_indices.size();

    // Dedup and query — use TopKCollector to cap merged results.
    // Capacity = offset + fetch_limit + 1 (extra one to detect next page).
    // Dict-level TopK (max_results_before_merge) limits per-path candidates.
    size_t topk_cap = (size_t)(offset + fetch_limit + 1);
    TopKCollector merged(topk_cap);

    // Seed the collector with fast-path candidates before fallback.
    if (fast.hit) {
        for (auto& c : fast.candidates) {
            if (!contains_text(merged.items(), c.text)) {
                Candidate copy = c;
                merged.offer(std::move(copy));
            }
        }
    }

    // Track processed ID sequences for dedup (small N, linear scan is fine)
    std::vector<std::vector<uint32_t>> processed_ids;

    std::chrono::steady_clock::time_point t_lookup_start, t_lookup_end;
    if (trace) t_lookup_start = std::chrono::steady_clock::now();

    for (size_t live_path_index : live_path_indices) {
        auto& ids = id_sequences[live_path_index];
        if (contains_ids(processed_ids, ids))
            continue;
        processed_ids.push_back(ids);
        // Check deadline before each lookup_by_ids
        if (budget && budget->deadline.expired()) {
            deadline_hit = true;
            if (trace) {
                trace->deadline_exceeded = true;
                trace->truncated = true;
            }
            break;
        }
        auto candidates = dict_->lookup_by_ids(ids, offset + fetch_limit + 1, trace, budget);
        for (auto& c : candidates) {
            rank_fallback_candidate(c, path_tiers[live_path_index], ids.size());
        }
        if (pinyin_scheme() == PinyinSchemeKind::kShuangpin) {
            rank_shuangpin_path(path_query_keys[live_path_index], candidates);
        }
        for (auto& c : candidates) {
            if (!contains_text(merged.items(), c.text))
                merged.offer(std::move(c));
        }
    }

    if (trace) {
        t_lookup_end = std::chrono::steady_clock::now();
        trace->lookup_us = std::chrono::duration_cast<std::chrono::microseconds>(t_lookup_end - t_lookup_start).count();
    }

    // finish() sorts by frequency descending
    std::chrono::steady_clock::time_point t_merge_start;
    if (trace) t_merge_start = std::chrono::steady_clock::now();

    auto sorted = merged.finish();
    if (trace) {
        trace->merge_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_merge_start).count();
    }

    if (sentence_composition_enabled_ && !composition_specs.empty() &&
        sorted.size() < static_cast<size_t>(need) &&
        !(budget && budget->deadline.expired())) {
        std::vector<CompositionPath> composition_paths;
        composition_paths.reserve(composition_specs.size());
        for (const auto& spec : composition_specs) {
            if (spec.id_sequence_index >= id_sequences.size() ||
                spec.segmented_path_index >= segment_result.paths.size()) {
                continue;
            }
            CompositionPath path;
            path.ids = &id_sequences[spec.id_sequence_index];
            path.syllables = &segment_result.paths[spec.segmented_path_index].syllables;
            if (!path_query_keys[spec.id_sequence_index].empty()) {
                path.input_code = &path_query_keys[spec.id_sequence_index];
            }
            path.kind = spec.kind;
            path.rank = spec.rank;
            composition_paths.push_back(path);
        }

        const auto composition_start = std::chrono::steady_clock::now();
        PinyinComposer composer(*dict_);
        CompositionLimits composition_limits;
        CompositionStats composition_stats;
        const QueryDeadline no_deadline;
        const QueryDeadline& composition_deadline = budget ? budget->deadline : no_deadline;
        auto composed = composer.compose(
            pinyin, composition_paths, static_cast<size_t>(need) - sorted.size(),
            composition_deadline, composition_limits, composition_stats);
        dict_->filter_disabled_system_candidates(composed);
        if (composition_stats.repeated_short_path_count > 0) {
            // Repeated-short composition consumes every key, so a legacy candidate that extends
            // an exact composed result contains text not covered by the input.
            remove_repeated_short_extensions(sorted, composed);
        }
        uint32_t appended_count = 0;
        for (auto& candidate : composed) {
            if (!contains_text(sorted, candidate.text)) {
                sorted.push_back(std::move(candidate));
                ++appended_count;
            }
        }

        if (trace) {
            trace->composition_path_count = composition_stats.normal_path_count +
                                            composition_stats.repeated_short_path_count;
            trace->composition_repeated_short_path_count =
                composition_stats.repeated_short_path_count;
            trace->span_query_count = composition_stats.span_query_count;
            trace->span_entry_scan_count = composition_stats.span_entry_scan_count;
            trace->composition_state_count = composition_stats.state_count;
            trace->composed_candidate_count = appended_count;
            trace->composition_truncated = composition_stats.truncated;
            trace->composition_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - composition_start).count();
            if (composition_stats.truncated) {
                trace->truncated = true;
            }
            if (composition_stats.deadline_exceeded) {
                trace->deadline_exceeded = true;
            }
        }
        if (composition_stats.deadline_exceeded) {
            deadline_hit = true;
        }
    }

    remove_oversized_candidates(sorted);
    keep_initials_matches(pinyin, sorted);

    if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin && candidate_learning_enabled_) {
        dict_->apply_candidate_preferences(pinyin, CandidateSource::kPinyin, sorted, need);
    }
    dict_->filter_disabled_system_candidates(sorted);
    if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin && candidate_learning_enabled_ &&
        composition_learning_) {
        auto learned = composition_learning_->lookup_candidates(pinyin, need);
        dict_->filter_disabled_system_candidates(learned);
        keep_initials_matches(pinyin, learned);
        for (auto& candidate : learned) {
            merge_candidate_by_score(sorted, std::move(candidate));
        }
        sort_candidates_by_score(sorted);
        if (static_cast<int>(sorted.size()) > need) {
            sorted.resize(need);
        }
    }
    if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin) {
        dict_->apply_manual_candidate_order(pinyin, CandidateSource::kPinyin, sorted, need);
    } else {
        std::vector<std::string> query_keys;
        for (const auto& query_key : path_query_keys) {
            if (!query_key.empty() &&
                std::find(query_keys.begin(), query_keys.end(), query_key) == query_keys.end()) {
                query_keys.push_back(query_key);
            }
        }
        for (auto it = query_keys.rbegin(); it != query_keys.rend(); ++it) {
            dict_->apply_manual_candidate_order(*it, CandidateSource::kPinyin, sorted, need, *it);
        }
    }

    const int known_count = static_cast<int>(sorted.size());
    const int returned_end = (std::min)(offset + fetch_limit, known_count);
    const bool collector_capacity_incomplete = budget &&
        budget->max_results_before_merge > 0 &&
        budget->max_results_before_merge < static_cast<uint32_t>(need) &&
        (!trace || trace->topk_truncated);
    const bool incomplete = deadline_hit ||
        (trace && (trace->deadline_exceeded || trace->scan_budget_truncated ||
                   trace->composition_truncated)) ||
        collector_capacity_incomplete;
    page.extent = make_candidate_extent(known_count, returned_end, incomplete);

    // Apply pagination
    if (offset >= (int)sorted.size()) {
        sorted.clear();
    } else if (offset > 0) {
        sorted.erase(sorted.begin(), sorted.begin() + offset);
    }
    if ((int)sorted.size() > fetch_limit)
        sorted.resize(fetch_limit);

    page.candidates = std::move(sorted);
    for (auto& c : page.candidates)
        c.source = CandidateSource::kPinyin;
    if (!page.candidates.empty())
        page.highlighted = 0;

    if (!require_runtime_paths && page.extent.complete) {
        store_query_cache(pinyin, page_index, offset, page_size, cache_versions, page);
    }

    return page;
}

TranslationResult PinyinTranslator::translate(const TranslationRequest& request) {
    TranslationResult result;
    if (!dict_ || !dict_->is_open() || request.input.empty() || request.page_size <= 0) {
        result.status = dict_ && dict_->is_open() ? TranslationStatus::kSuccess
                                                  : TranslationStatus::kFailed;
        return result;
    }

    if (!request.policy.allow_partial_selection) {
        const bool fuzzy_paths = pinyin_resources_ && pinyin_query_policy_.enable_fuzzy &&
                                 pinyin_resources_->has_fuzzy_path(request.input, fuzzy_options());
        CandidatePage page = translate_page(
            request.input, request.page_index, request.page_size, request.trace,
            request.budget, request.scratch, request.page_offset, fuzzy_paths);
        result = make_translation_result(std::move(page), request.input.size());
        annotate_fuzzy_matches(request.input, result.entries, 0,
                               static_cast<int>(result.entries.size()));
        const bool incomplete = (request.trace &&
                                (request.trace->deadline_exceeded ||
                                 request.trace->scan_budget_truncated ||
                                 request.trace->composition_truncated)) ||
                                !result.extent.complete;
        if (incomplete) {
            result.status = result.entries.empty() ? TranslationStatus::kFailed
                                                   : TranslationStatus::kStableDegraded;
        }
        return result;
    }

    QueryTrace local_trace;
    TranslationRequest effective_request = request;
    if (!effective_request.trace) {
        effective_request.trace = &local_trace;
    }
    effective_request.trace->deadline_exceeded = false;
    effective_request.trace->scan_budget_truncated = false;
    effective_request.trace->topk_truncated = false;
    effective_request.trace->composition_truncated = false;

    // Only the leading full-span group must be materialized before partial candidates. Fetching
    // enough trailing full-span entries for every possible partial up front spends the latency
    // budget on candidates ranked after the partial group.
    const int fetch_count = (std::max)(
        request.page_offset + request.page_size + 1,
        static_cast<int>(kLeadingFullSpanCandidateCount + 1));
    const bool require_runtime_paths =
        pinyin_resources_ && pinyin_query_policy_.enable_fuzzy &&
        pinyin_resources_->has_fuzzy_path(request.input, fuzzy_options());
    CandidatePage full = translate_page(request.input, 0, fetch_count, effective_request.trace,
                                        request.budget, request.scratch, 0,
                                        require_runtime_paths);
    std::vector<CandidateEntry> merged;
    merged.reserve(full.candidates.size() + request.page_size);
    for (auto& candidate : full.candidates) {
        merged.push_back(make_text_candidate_entry(std::move(candidate), request.input.size()));
    }
    const bool full_query_incomplete = effective_request.trace->deadline_exceeded ||
                                       effective_request.trace->scan_budget_truncated ||
                                       effective_request.trace->composition_truncated ||
                                       !full.extent.complete;
    if (full_query_incomplete) {
        result.status = merged.empty() ? TranslationStatus::kFailed
                                       : TranslationStatus::kStableDegraded;
    }
    const std::size_t full_count = merged.size();
    if (pinyin_resources_) {
        append_pinyin_partial_candidates(*dict_, *pinyin_resources_, pinyin_query_policy_,
                                         effective_request,
                                         pinyin_scheme() == PinyinSchemeKind::kShuangpin,
                                         candidate_learning_enabled_, merged, result.status);
    }
    const std::size_t added_partial_count = merged.size() - full_count;
    const auto is_partial_entry = [&](const CandidateEntry& entry) {
        const auto* action = std::get_if<TextSelectionAction>(&entry.selection);
        return action && action->consumed_input_bytes < request.input.size();
    };
    const std::size_t visible_partial_count =
        static_cast<std::size_t>(std::count_if(merged.begin(), merged.end(), is_partial_entry));
    const std::size_t visible_full_count = merged.size() - visible_partial_count;
    const std::size_t leading_full_count = (std::min)(
        visible_full_count, kLeadingFullSpanCandidateCount);

    // Keep a fixed number of leading full-span choices, then group all partials
    // before lower-ranked full-span choices. The order is independent of page size.
    if (visible_full_count > leading_full_count && visible_partial_count > 0) {
        std::vector<CandidateEntry> partials;
        std::vector<CandidateEntry> fulls;
        partials.reserve(visible_partial_count);
        fulls.reserve(visible_full_count);
        for (auto& entry : merged) {
            if (is_partial_entry(entry)) {
                partials.push_back(std::move(entry));
            } else {
                fulls.push_back(std::move(entry));
            }
        }
        merged.clear();
        merged.reserve(fulls.size() + partials.size());

        for (std::size_t index = 0; index < leading_full_count; ++index) {
            merged.push_back(std::move(fulls[index]));
        }

        std::size_t full_index = leading_full_count;
        std::size_t partial_index = 0;
        while (partial_index < partials.size()) {
            merged.push_back(std::move(partials[partial_index++]));
        }
        while (full_index < fulls.size()) {
            merged.push_back(std::move(fulls[full_index++]));
        }
    }

    result.page_index = request.page_index;
    result.page_offset = request.page_offset;
    result.page_size = request.page_size;
    const int available = static_cast<int>(merged.size());
    const int page_begin = (std::min)(request.page_offset, available);
    const int page_end = (std::min)(page_begin + request.page_size, available);
    annotate_fuzzy_matches(request.input, merged, page_begin, page_end);
    const int begin = (std::min)(request.page_offset, available);
    const int end = (std::min)(begin + request.page_size, available);
    const int known_count = (std::max)(
        available, full.extent.known_count + static_cast<int>(added_partial_count));
    const bool incomplete = result.status != TranslationStatus::kSuccess ||
                            !full.extent.complete;
    result.extent = make_candidate_extent(known_count, end, incomplete);
    result.entries.assign(std::make_move_iterator(merged.begin() + begin),
                          std::make_move_iterator(merged.begin() + end));
    if (!result.entries.empty()) {
        result.highlighted = 0;
    }
    return result;
}

} // namespace cxxime
