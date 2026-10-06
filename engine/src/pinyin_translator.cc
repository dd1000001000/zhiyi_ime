// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/translator.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <functional>
#include <iterator>
#include <numeric>
#include <string_view>
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
// Keep the ranking aligned with scripts/build_pinyin_topn.py (key_score), as Rime ranks phrases:
//   complete    every syllable matched, in full or by its initial;
//   completion  the last syllable completed ("ni" -> 年), or initials used although the input reads
//               as whole syllables ("zhou" -> 最后 as z+hou);
//   extension   the word is longer than the input (先占领 for "xianzhan").
// Within a group: ln(frequency) plus the path's spelling credibility (ln 0.5 for each abbreviated
// or completed syllable). User dictionary matches start at 120,000,000.
constexpr int kCompleteBase = 60000000;
constexpr int kCompletionBase = 45000000;
constexpr int kExtensionBase = 30000000;
// A character the dictionary has hardly seen used (frequency up to 100: 㝵, 給, 瘧, 祋) goes below
// every word and sentence, at a hundredth of its log score (under 150,000).
constexpr int kRareCharacterFrequency = 100;
constexpr int kRareCharacterScale = 100;
constexpr double kLogScale = 500000.0;
constexpr double kLogOffset = 10.0;
constexpr double kMaxLogScore = 14999999.0;
constexpr float kAbbreviationCredibility = -0.6931472f;  // ln(0.5)
// A completion (the word goes on beyond the typed letters) is read with Rime's ln(0.05): the
// path already carries ln(0.5) for the cut syllable, this adds the rest. Words at the
// dictionary's floor weight (the imported long tail) neither complete nor extend an input that
// uses an initial: 你看行不行 does not answer "nikanx" (x alone), while 别错过 still completes
// "biecuogu" and 公理会 "gonglihu", where the last syllable is only cut short.
constexpr float kCompletionExtraCredibility = -2.3025851f;  // ln(0.1)
constexpr int kMinCompletionFrequency = 100;
// Fuzzy pinyin matches (xian -> 想 through an=ang): on top of the fuzzy spelling's ln(0.5), so a
// fuzzy word counts about a tenth of its frequency and only passes a far less common exact one.
constexpr float kFuzzyExtraCredibility = -1.6094379f;  // ln(0.2)

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

// An initial typed before a full syllable ("wsyige": w, s, then yi, ge). Each initial expands
// to all its syllables, and the enumerated paths (kMaxPaths) hold only the most plausible
// combinations; such inputs are also looked up by their initials (lookup_mixed_by_initials).
bool has_initial_before_full_syllable(const SegmentedPath& path) {
    bool seen_abbreviation = false;
    for (uint8_t type : path.spelling_types) {
        if (type == kAbbreviation) {
            seen_abbreviation = true;
        } else if (seen_abbreviation && type <= kFuzzySpelling) {
            return true;
        }
    }
    return false;
}

bool path_uses_initial(const SegmentedPath& path) {
    return std::any_of(path.spelling_types.begin(), path.spelling_types.end(),
                       [](uint8_t type) { return type == kAbbreviation; });
}

bool path_uses_fuzzy(const SegmentedPath& path) {
    return std::any_of(path.spelling_types.begin(), path.spelling_types.end(),
                       [](uint8_t type) { return type == kFuzzySpelling; });
}

// Bit i set when syllable i is a 儿 typed as r (the erhua edge: "er" from one letter).
uint32_t erhua_mask(const SegmentedPath& path) {
    uint32_t mask = 0;
    for (size_t i = 0; i < path.syllables.size() && i < 32 && i < path.input_lengths.size(); ++i) {
        if (path.syllables[i] == "er" && path.input_lengths[i] == 1) {
            mask |= 1u << i;
        }
    }
    return mask;
}

// The syllable "er" typed as r is 儿 only (花儿, not 华尔 or 十二): drops the candidates whose
// character at an erhua position is another one.
void keep_erhua_characters(std::vector<Candidate>& candidates, uint32_t mask) {
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [&](const Candidate& candidate) {
                           const std::string& text = candidate.text;
                           size_t index = 0;
                           for (size_t pos = 0; pos < text.size() && index < 32;) {
                               const unsigned char lead = static_cast<unsigned char>(text[pos]);
                               const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2
                                                     : lead < 0xF0 ? 3 : 4;
                               if ((mask & (1u << index)) &&
                                   text.compare(pos, length, "儿") != 0) {
                                   return true;
                               }
                               pos += length;
                               ++index;
                           }
                           return false;
                       }),
        candidates.end());
}

std::string initials_key(const SegmentedPath& path) {
    std::string key;
    for (const std::string& syllable : path.syllables) {
        if (syllable.empty()) return {};
        key.push_back(syllable[0]);
    }
    return key;
}

// One UTF-8 encoded character (up to four bytes).
bool is_single_character(const std::string& text) {
    if (text.empty() || text.size() > 4) return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return false;
    }
    return true;
}

std::size_t candidate_syllable_count(const Candidate& candidate) {
    if (candidate.syllables.empty()) {
        return 0;
    }
    return 1 + static_cast<std::size_t>(
                   std::count(candidate.syllables.begin(), candidate.syllables.end(), ':'));
}

// False when the candidate is dropped (a floor-weight word reached through an initial).
bool rank_fallback_candidate(Candidate& candidate, PathMatchTier tier,
                             std::size_t query_syllable_count, float credibility,
                             bool input_reads_as_syllables, bool path_abbreviated,
                             bool path_fuzzy) {
    int base = kCompleteBase;
    if (candidate_syllable_count(candidate) != query_syllable_count) {
        base = kExtensionBase;
    } else if (tier == PathMatchTier::kCompletion ||
               (input_reads_as_syllables &&
                (tier == PathMatchTier::kAbbreviation || tier == PathMatchTier::kMixed))) {
        base = kCompletionBase;
    } else if (path_fuzzy && path_abbreviated) {
        // A word that needs both a fuzzy spelling and an initial to cover the input (李康勋 for
        // "nikanx" with n/l fuzzy) ranks with the longer words, after the sentences read as
        // typed (你看+下).
        base = kExtensionBase;
    }
    if (tier == PathMatchTier::kFuzzy) {
        credibility += kFuzzyExtraCredibility;
    }
    if (base != kCompleteBase && path_abbreviated &&
        candidate.source_frequency <= kMinCompletionFrequency) {
        return false;
    }
    if (tier == PathMatchTier::kCompletion) {
        credibility += kCompletionExtraCredibility;
    }
    const double frequency = static_cast<double>((std::max)(0, candidate.source_frequency));
    const double log_score = (std::min)(
        kMaxLogScore,
        (std::max)(0.0, std::round(kLogScale * (std::log(frequency + 1.0) + credibility +
                                                kLogOffset))));
    if (candidate.source_frequency <= kRareCharacterFrequency &&
        is_single_character(candidate.text)) {
        candidate.frequency = static_cast<int>(log_score) / kRareCharacterScale;
        return true;
    }
    candidate.frequency = base + static_cast<int>(log_score);
    return true;
}

// How credibly the span `typed` spells exactly these syllables, each in full or by its initial
// (z / zh): the span check of compose_typed_spans. Returns the credibility in thousandths of a
// natural log (0 = typed in full), or INT64_MIN when the span does not spell the word.
//   ln 0.5   for every initial, as the abbreviation spellings;
//   ln 0.05  more when the letters after the initial go on to spell the syllable in full
//            (看 read as "k" in "nikanx" although "kan" is typed): Rime's syllabifier drops such
//            abbreviation edges, as the typist meant the whole syllable;
//   ln 0.1   more for a single character read by its initial alone, the weakest reading
//            (维生素 + 把 for "wssb" must not outrank 晚上 + 上班);
//   0        for a final 儿 typed as r (花儿 "huar"), the erhua spelling.
int64_t typed_span_credibility(std::string_view typed, std::string_view syllables) {
    constexpr int64_t kInitial = -693;         // ln(0.5)
    constexpr int64_t kShadowedInitial = -2996;  // ln(0.05)
    constexpr int64_t kLoneInitial = -2303;    // ln(0.1)
    std::vector<std::string_view> parts;
    while (!syllables.empty()) {
        const std::size_t colon = syllables.find(':');
        parts.push_back(syllables.substr(0, colon));
        if (colon == std::string_view::npos) break;
        syllables.remove_prefix(colon + 1);
    }
    int64_t best = INT64_MIN;
    std::function<void(std::size_t, std::size_t, int64_t)> walk =
        [&](std::size_t i, std::size_t p, int64_t credibility) {
            if (i == parts.size() || p == typed.size()) {
                if (i == parts.size() && p == typed.size()) best = (std::max)(best, credibility);
                return;
            }
            const std::string_view s = parts[i];
            const std::string_view rest = typed.substr(p);
            const bool typed_in_full = rest.substr(0, s.size()) == s;
            if (typed_in_full) walk(i + 1, p + s.size(), credibility);
            if (i > 0 && i + 1 == parts.size() && s == "er" && !rest.empty() && rest[0] == 'r') {
                walk(i + 1, p + 1, credibility);
            }
            const bool retroflex =
                s.size() > 2 && s[1] == 'h' && (s[0] == 'z' || s[0] == 'c' || s[0] == 's');
            for (std::size_t length = 1; length <= (retroflex ? 2u : 1u); ++length) {
                if (length < s.size() && rest.substr(0, length) == s.substr(0, length)) {
                    walk(i + 1, p + length,
                         credibility + kInitial + (typed_in_full ? kShadowedInitial : 0));
                }
            }
        };
    walk(0, 0, 0);
    if (best != INT64_MIN && parts.size() == 1 && typed.size() < parts[0].size()) {
        best += kLoneInitial;
    }
    return best;
}

// Whether the input splits into whole syllables (normal or fuzzy spellings), as Rime's
// syllabifier decides to drop abbreviations.
bool reads_as_syllables(const std::string& input, const std::vector<SegmentedPath>& paths) {
    return std::any_of(paths.begin(), paths.end(), [&](const SegmentedPath& path) {
        return path_consumes_entire_input(input, path) &&
               std::all_of(path.spelling_types.begin(), path.spelling_types.end(),
                           [](uint8_t type) { return type <= kFuzzySpelling; });
    });
}

// Spelling credibility of reading `input` as these syllables, each in full or as its initial
// (z / zh): ln(0.5) per initial, as build_pinyin_topn.py key_quality.
float typed_credibility(const std::string& input, const std::string& syllables) {
    std::vector<std::string_view> parts;
    std::string_view rest(syllables);
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        parts.push_back(rest.substr(0, colon));
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    float best = -1e9f;
    std::function<void(std::size_t, std::size_t, float)> walk =
        [&](std::size_t i, std::size_t p, float credibility) {
            if (p == input.size() || i == parts.size()) {
                if (p == input.size() && i == parts.size()) best = (std::max)(best, credibility);
                return;
            }
            const std::string_view s = parts[i];
            const std::string_view typed(input.data() + p, input.size() - p);
            if (typed.substr(0, s.size()) == s) walk(i + 1, p + s.size(), credibility);
            // Its initial: the first letter, or zh / ch / sh.
            const bool retroflex =
                s.size() > 2 && s[1] == 'h' && (s[0] == 'z' || s[0] == 'c' || s[0] == 's');
            for (std::size_t length = 1; length <= (retroflex ? 2u : 1u); ++length) {
                if (length < s.size() && typed.substr(0, length) == s.substr(0, length)) {
                    walk(i + 1, p + length, credibility + kAbbreviationCredibility);
                }
            }
        };
    walk(0, 0, 0.0f);
    return best < -1e8f ? 0.0f : best;
}

// Leading sentence for an input whose last syllable is unfinished (see translate()).
constexpr std::size_t kMinLeadingSentenceInput = 6;
constexpr std::size_t kMaxUnfinishedLetters = 4;
constexpr int kLeadingSentenceFetch = 4;

// Whether the syllables spell the input in full (ü typed as v allowed): the candidate covers
// exactly these letters, not a longer word they only start.
bool spells_input(const std::string& input, const std::string& syllables) {
    std::size_t position = 0;
    for (const char c : syllables) {
        if (c == ':') {
            continue;
        }
        if (position >= input.size() ||
            (input[position] != c && !(input[position] == 'v' && c == 'u'))) {
            return false;
        }
        ++position;
    }
    return position == input.size();
}

constexpr size_t kMaxTypedSpanSentences = 12;

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
    float credibility = 0.0f;
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

// Inserts before the first lower-scored item: the existing order (ties included) stays as it is.
static void insert_by_score(std::vector<Candidate>& items, Candidate candidate) {
    const auto position = std::find_if(items.begin(), items.end(), [&](const Candidate& item) {
        return item.frequency < candidate.frequency;
    });
    items.insert(position, std::move(candidate));
}

static void sort_candidates_by_score(std::vector<Candidate>& items) {
    std::sort(items.begin(), items.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.frequency != b.frequency) return a.frequency > b.frequency;
            if (a.text.size() != b.text.size()) return a.text.size() < b.text.size();
            return a.text < b.text;
        });
}

// Words longer than the input (半导体产业, 半导体激光器... for "bandaoti") are predictions: a page
// of them hides the shorter readings the typist may want (半岛, 半), so only the best few stay.
// Rime has no such cap, but its dictionary holds few long phrases; ours imports the long tail.
constexpr int kMaxExtensionCandidates = 3;

static void limit_extension_candidates(std::vector<Candidate>& candidates) {
    int extensions = 0;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const Candidate& candidate) {
                                        if (candidate.frequency < kExtensionBase ||
                                            candidate.frequency >= kCompletionBase) {
                                            return false;
                                        }
                                        return ++extensions > kMaxExtensionCandidates;
                                    }),
                     candidates.end());
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
        // A short Top-N list is not trusted even when marked complete: mixed keys such as
        // "zhongwe" can hold a single long phrase (中文歌曲) while the runtime lookup finds
        // 中文 / 中卫 / ... through terminal completion.
        if (require_runtime_paths || !fast.complete_index_hit ||
            static_cast<int>(fast.candidates.size()) < need) {
            return false;
        }
        auto& sorted = fast.candidates;
        limit_extension_candidates(sorted);
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
    std::vector<float> path_credibilities;
    std::vector<std::string> path_query_keys;
    std::vector<uint8_t> path_abbreviated;  // the path reads some syllable by its initial
    std::vector<uint8_t> path_fuzzy;        // the path reads some syllable by a fuzzy spelling
    std::vector<uint32_t> path_erhua;       // bit i: syllable i is a 儿 typed as r

    // The syllable starting with `letter` whose tripled word, then doubled word, is the most
    // common in the dictionary ("h": 哈哈哈 90k beats 呵呵呵 100): the reading of a repeated
    // short code.
    auto best_repeated_reading = [&](char letter) -> std::string {
        std::string best;
        std::pair<int, int> best_score{-1, -1};
        const SegmentResult readings = pinyin_resources_->segment(std::string(1, letter));
        for (const SegmentedPath& path : readings.paths) {
            if (path.syllables.size() != 1) continue;
            const std::string& syllable = path.syllables.front();
            const auto tripled = dict_->lookup_by_syllables({syllable, syllable, syllable}, 1);
            const auto doubled = dict_->lookup_by_syllables({syllable, syllable}, 1);
            const std::pair<int, int> score{tripled.empty() ? 0 : tripled.front().frequency,
                                            doubled.empty() ? 0 : doubled.front().frequency};
            if (score > best_score) {
                best_score = score;
                best = syllable;
            }
        }
        return best;
    };

    auto add_path = [&](const std::vector<std::string>& syllables,
                        PathMatchTier tier, float credibility,
                        std::string query_key = {}, bool abbreviated = false,
                        bool fuzzy = false, uint32_t erhua = 0) -> size_t {
        if (syllables.empty()) return SIZE_MAX;
        std::vector<uint32_t> ids;
        for (auto& s : syllables) {
            uint32_t id = dict_->syllable_to_id(s);
            if (id == UINT32_MAX) return SIZE_MAX;
            ids.push_back(id);
        }
        id_sequences.push_back(std::move(ids));
        path_tiers.push_back(tier);
        path_credibilities.push_back(credibility);
        path_query_keys.push_back(std::move(query_key));
        path_abbreviated.push_back(abbreviated ? 1 : 0);
        path_fuzzy.push_back(fuzzy ? 1 : 0);
        path_erhua.push_back(erhua);
        return id_sequences.size() - 1;
    };

    // 1. Syllabifier for abbreviation expansion (reserve first)
    // Limit paths to avoid CPU cache thrashing on short inputs (e.g. single letter 's')
    // The syllabifier checks the deadline internally, so it does not need to be skipped.
    // The syllabifier reports the most plausible paths first; two initials after full pinyin
    // ("womenjd": j, d) have a few hundred readings, and the wanted one (jue:de) is not
    // always among the first 64.
    static constexpr size_t kMaxPaths = 128;
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
    bool has_normal_composition_path = false;
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
                    segmented_path.credibility,
                    pinyin_scheme() == PinyinSchemeKind::kShuangpin
                        ? canonical_pinyin_key(segmented_path.syllables)
                        : std::string{},
                    path_uses_initial(segmented_path), path_uses_fuzzy(segmented_path),
                    erhua_mask(segmented_path));
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
                // The repeated letters read as the syllable whose reduplicated word is the most
                // common (哈哈哈, 哈哈 for h): "hhhh" is 哈哈哈哈, not 会会会会 although 会 is the
                // more common character and its path the first enumerated.
                const SegmentedPath& typed_path =
                    segment_result.paths[repeated_short_spec.segmented_path_index];
                size_t suffix_begin = typed_path.syllables.size();
                while (suffix_begin > 0 &&
                       typed_path.spelling_types[suffix_begin - 1] == kAbbreviation) {
                    --suffix_begin;
                }
                const std::string reading = best_repeated_reading(pinyin.back());
                if (!reading.empty() && suffix_begin < typed_path.syllables.size() &&
                    reading != typed_path.syllables[suffix_begin]) {
                    SegmentedPath path = typed_path;
                    for (size_t s = suffix_begin; s < path.syllables.size(); ++s) {
                        path.syllables[s] = reading;
                    }
                    const size_t id_index = add_path(path.syllables, classify_path(path),
                                                     path.credibility, {}, true,
                                                     path_uses_fuzzy(path));
                    if (id_index != SIZE_MAX) {
                        segment_result.paths.push_back(std::move(path));
                        const size_t path_index = segment_result.paths.size() - 1;
                        repeated_short_spec = {id_index, path_index,
                                               CompositionPathKind::kRepeatedShortCode,
                                               static_cast<uint16_t>(path_index)};
                    }
                }
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

    const bool input_is_syllables = reads_as_syllables(pinyin, segment_result.paths);

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
                existing->credibility = (std::max)(existing->credibility, path.credibility);
                continue;
            }
            indexed_paths.push_back({key, tier, path.syllables.size(), path.credibility});
            if (indexed_paths.size() >= kMaxIndexedShuangpinPaths) {
                break;
            }
        }
        for (const auto& path : indexed_paths) {
            IndexedFastResult path_fast = lookup_indexed_fast(path.key, need, trace);
            for (auto& candidate : path_fast.candidates) {
                candidate.input_code = path.key;
            }
            if (path.tier != PathMatchTier::kNormal) {
                path_fast.candidates.erase(
                    std::remove_if(path_fast.candidates.begin(), path_fast.candidates.end(),
                                   [&](Candidate& candidate) {
                                       return !rank_fallback_candidate(
                                           candidate, path.tier, path.syllable_count,
                                           path.credibility, input_is_syllables,
                                           path.tier == PathMatchTier::kAbbreviation ||
                                               path.tier == PathMatchTier::kMixed,
                                           false);
                                   }),
                    path_fast.candidates.end());
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
        add_path(segmentor_.segment_best(pinyin), PathMatchTier::kNormal, 0.0f);

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

    // Terminal syllable completion ("ji" -> "jie", "buzhida" -> bu:zhi:dao) is always tried,
    // as the Top-N index completes every key it holds: the completed words rank in the
    // completion group, under the words the typed syllables spell in full (不知大) and above the
    // longer words (不知大体). The syllabifier only adds the paths that end in a completion.
    if (pinyin_resources_ && !deadline_hit && !pinyin_query_policy_.initials_only) {
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
                if (completion_path.spelling_types.empty() ||
                    completion_path.spelling_types.back() != kCompletionSpelling) {
                    continue;  // already enumerated without completion
                }
                add_path(completion_path.syllables, classify_path(completion_path),
                         completion_path.credibility,
                         pinyin_scheme() == PinyinSchemeKind::kShuangpin
                             ? canonical_pinyin_key(completion_path.syllables)
                             : std::string{},
                         path_uses_initial(completion_path), path_uses_fuzzy(completion_path),
                         erhua_mask(completion_path));
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
        if (path_erhua[live_path_index] != 0) {
            keep_erhua_characters(candidates, path_erhua[live_path_index]);
        }
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](Candidate& c) {
                                            return !rank_fallback_candidate(
                                                c, path_tiers[live_path_index], ids.size(),
                                                path_credibilities[live_path_index],
                                                input_is_syllables,
                                                path_abbreviated[live_path_index] != 0,
                                                path_fuzzy[live_path_index] != 0);
                                        }),
                         candidates.end());
        if (pinyin_scheme() == PinyinSchemeKind::kShuangpin) {
            rank_shuangpin_path(path_query_keys[live_path_index], candidates);
        }
        // A word reached through several paths keeps its best rank (exact over fuzzy).
        for (auto& c : candidates) {
            merged.offer_unique(std::move(c));
        }
    }

    // Full pinyin mixed with initials typed first ("wsyige" -> 我是一个): the words under the
    // initials ("wsyg") whose syllables fit the input, full where typed in full.
    if (pinyin_scheme() == PinyinSchemeKind::kFullPinyin && !pinyin_query_policy_.initials_only &&
        !deadline_hit) {
        static constexpr size_t kMaxMixedKeys = 3;
        static constexpr int kMixedFetch = 64;
        std::vector<std::string> keys;
        for (size_t i = 0; i < segment_result.paths.size() && keys.size() < kMaxMixedKeys; ++i) {
            const auto& path = segment_result.paths[i];
            if (!path_consumes_entire_input(pinyin, path) ||
                !has_initial_before_full_syllable(path)) {
                continue;
            }
            std::string key = initials_key(path);
            if (key.size() < 2 || std::find(keys.begin(), keys.end(), key) != keys.end()) {
                continue;
            }
            keys.push_back(std::move(key));
        }
        for (const std::string& key : keys) {
            IndexedFastResult found = lookup_indexed_fast(key, kMixedFetch, nullptr);
            for (auto& candidate : found.candidates) {
                if (!pinyin_matches_mixed(pinyin, candidate.syllables)) {
                    continue;
                }
                if (!rank_fallback_candidate(candidate, PathMatchTier::kMixed, key.size(),
                                             typed_credibility(pinyin, candidate.syllables),
                                             input_is_syllables, true, false)) {
                    continue;
                }
                candidate.input_code = pinyin;
                merged.offer_unique(std::move(candidate));
            }
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
    // A dictionary word covers the whole input (complete or by completion): sentences then go
    // below every word (covered_sentence_frequency). When the word is typed in full ("nihao")
    // no typed-span sentences are built either; a word reached through an initial ("womenjd" ->
    // 我们决定) still gets the typed-span sentences (我们觉得) after the words.
    const bool input_covered = std::any_of(sorted.begin(), sorted.end(), [](const Candidate& c) {
        return c.frequency >= kCompletionBase;
    });
    const bool input_covered_in_full =
        std::any_of(sorted.begin(), sorted.end(), [&](const Candidate& c) {
            if (c.frequency < kCompletionBase) {
                return false;
            }
            std::string spelled;
            for (const char ch : c.syllables) {
                if (ch != ':') spelled.push_back(ch);
            }
            return spelled == pinyin;
        });

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
                if (input_covered) {
                    candidate.frequency = covered_sentence_frequency(candidate.frequency);
                }
                insert_by_score(sorted, std::move(candidate));
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

    // Initials mixed into the input ("rangwolaibsmoxing", "zhegemx", "nikanx"): no syllable
    // path is typed in full, so the composer above has nothing to do. Build the sentences over
    // the typed letters instead, unless a dictionary word typed in full covers the input, as
    // Rime drops its sentence when a phrase spans the whole input; a word reached through an
    // initial (我们决定 for "womenjd") keeps them after every word.
    // A repeated letter at the end ("hhhh", "nihh") is the repeated short code the composer
    // above handles, one character per letter; spans would pair the letters into words
    // (呵呵 + 哈哈哈哈哈哈哈) instead.
    const bool repeated_trailing_letter =
        pinyin.size() >= 2 && pinyin[pinyin.size() - 1] == pinyin[pinyin.size() - 2];
    bool typed_span_composed = false;
    if (sentence_composition_enabled_ && pinyin_scheme() == PinyinSchemeKind::kFullPinyin &&
        !pinyin_query_policy_.initials_only && !has_normal_composition_path && !deadline_hit &&
        !repeated_trailing_letter && !input_covered_in_full && pinyin.size() >= 2 &&
        is_indexable_key(pinyin)) {
        const auto span_start = std::chrono::steady_clock::now();
        auto sentences = compose_typed_spans(pinyin, kMaxTypedSpanSentences, budget);
        dict_->filter_disabled_system_candidates(sentences);
        for (auto& candidate : sentences) {
            if (!contains_text(sorted, candidate.text)) {
                if (input_covered) {
                    candidate.frequency = covered_sentence_frequency(candidate.frequency);
                }
                insert_by_score(sorted, std::move(candidate));
                typed_span_composed = true;
            }
        }
        if (trace) {
            trace->composition_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - span_start).count();
        }
    }
    if ((typed_span_composed || !composition_specs.empty()) &&
        static_cast<int>(sorted.size()) > need) {
        sorted.resize(need);
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

    limit_extension_candidates(sorted);
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

// Sentences over the typed letters. Every span input[start, end) is looked up in the Top-N
// index (whose keys include initials and mixed spellings: "bs", "mx", "x", "rangwolai"); the
// words the span spells exactly, in full or by initials, become edges scored as the Poet does
// (composed_word_score, ln 0.5 per initial). A beam over the letter positions keeps the best
// sentences of two or more words; one word covering everything is a dictionary match already.
std::vector<Candidate> PinyinTranslator::compose_typed_spans(const std::string& input,
                                                             size_t max_results,
                                                             const QueryBudget* budget) const {
    struct Edge {
        size_t end = 0;
        int64_t score = 0;
        int frequency = 0;
        const Candidate* word = nullptr;
    };
    struct State {
        int64_t score = 0;
        int frequency = 0;  // the weakest word
        uint16_t words = 0;
        uint32_t edge = UINT32_MAX;  // the last edge, in `edges`
        uint32_t parent = UINT32_MAX;  // the state before it, in `states`
    };
    static constexpr size_t kMaxSpanLetters = 14;
    static constexpr int kSpanFetch = 32;
    // Words kept per span: the initials of a span ("bs") fit many words, and the context model
    // chooses among the sentences, so it sees more than the Poet's few homophones.
    static constexpr size_t kMaxWordsPerSpan = 12;
    static constexpr int kMaxSingleCharacterTails = 2;
    static constexpr size_t kBeamWidth = 16;
    static constexpr size_t kMaxWords = 6;
    static constexpr int64_t kSentenceWindow = 4605;  // ln(100): sentences this far behind the best

    const size_t n = input.size();
    std::vector<std::vector<Candidate>> words_by_span;  // owns the words the edges point to
    std::vector<std::vector<Edge>> edges_from(n);
    for (size_t start = 0; start < n; ++start) {
        for (size_t end = start + 1; end <= n && end - start <= kMaxSpanLetters; ++end) {
            if (start == 0 && end == n) continue;  // a single word: the dictionary's own match
            if (budget && budget->deadline.expired()) break;
            const std::string key = input.substr(start, end - start);
            IndexedFastResult found = lookup_indexed_fast(key, kSpanFetch, nullptr);
            if (found.candidates.empty()) continue;
            std::vector<Candidate> words;
            std::vector<Edge> span_edges;
            for (auto& candidate : found.candidates) {
                const int64_t credibility =
                    candidate.syllables.empty() ? INT64_MIN
                                                : typed_span_credibility(key, candidate.syllables);
                if (credibility == INT64_MIN) {
                    continue;
                }
                if (std::any_of(words.begin(), words.end(),
                                [&](const Candidate& w) { return w.text == candidate.text; })) {
                    continue;
                }
                Edge edge;
                edge.end = end;
                edge.frequency = candidate.source_frequency;
                edge.score = composed_word_score(candidate.source_frequency) + credibility;
                words.push_back(std::move(candidate));
                span_edges.push_back(edge);
            }
            if (words.empty()) continue;
            std::vector<size_t> order(span_edges.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return span_edges[a].score > span_edges[b].score;
            });
            words_by_span.push_back(std::move(words));
            for (size_t i = 0; i < order.size() && i < kMaxWordsPerSpan; ++i) {
                Edge edge = span_edges[order[i]];
                edge.word = &words_by_span.back()[order[i]];
                edges_from[start].push_back(edge);
            }
        }
    }

    std::vector<State> states;
    std::vector<std::vector<uint32_t>> beam(n + 1);  // states ending at each position
    states.push_back(State{});
    beam[0].push_back(0);
    std::vector<Edge> all_edges;
    for (size_t pos = 0; pos < n; ++pos) {
        if (beam[pos].empty()) continue;
        for (uint32_t state_index : beam[pos]) {
            const State from = states[state_index];
            if (from.words >= kMaxWords) continue;
            for (const Edge& edge : edges_from[pos]) {
                State next;
                next.score = from.score + edge.score;
                next.frequency = from.words == 0 ? edge.frequency
                                                 : (std::min)(from.frequency, edge.frequency);
                next.words = static_cast<uint16_t>(from.words + 1);
                next.edge = static_cast<uint32_t>(all_edges.size());
                next.parent = state_index;
                all_edges.push_back(edge);
                states.push_back(next);
                beam[edge.end].push_back(static_cast<uint32_t>(states.size() - 1));
            }
        }
        // The next positions keep only their best states (fewer words first on a tie). States
        // that differ only in a final single character are the homophones of a lone initial
        // (我们将+的, 我们将+到, 我们将+大...): two per parent, so the other readings of the
        // input (我们+觉得) stay within the beam.
        for (size_t later = pos + 1; later <= n; ++later) {
            auto& bucket = beam[later];
            if (bucket.size() <= kBeamWidth) continue;
            std::stable_sort(bucket.begin(), bucket.end(), [&](uint32_t a, uint32_t b) {
                if (states[a].score != states[b].score) return states[a].score > states[b].score;
                return states[a].words < states[b].words;
            });
            std::vector<uint32_t> kept;
            struct Tails {
                uint32_t parent;
                int single_characters = 0;
            };
            std::vector<Tails> tails_by_parent;
            for (uint32_t state_index : bucket) {
                if (kept.size() >= kBeamWidth) break;
                const State& state = states[state_index];
                auto tails = std::find_if(tails_by_parent.begin(), tails_by_parent.end(),
                                          [&](const Tails& t) { return t.parent == state.parent; });
                if (tails == tails_by_parent.end()) {
                    tails_by_parent.push_back({state.parent});
                    tails = tails_by_parent.end() - 1;
                }
                if (all_edges[state.edge].word->text.size() <= 4) {  // one UTF-8 character
                    if (tails->single_characters >= kMaxSingleCharacterTails) continue;
                    ++tails->single_characters;
                }
                kept.push_back(state_index);
            }
            bucket = std::move(kept);
        }
    }

    std::vector<uint32_t> finals;
    for (uint32_t state_index : beam[n]) {
        if (states[state_index].words >= 2) finals.push_back(state_index);
    }
    std::stable_sort(finals.begin(), finals.end(), [&](uint32_t a, uint32_t b) {
        if (states[a].score != states[b].score) return states[a].score > states[b].score;
        return states[a].words < states[b].words;
    });

    std::vector<Candidate> results;
    for (uint32_t state_index : finals) {
        if (results.size() >= max_results) break;
        const State& final_state = states[state_index];
        if (!results.empty() && final_state.score < states[finals.front()].score - kSentenceWindow) {
            break;
        }
        std::vector<const Candidate*> chain;
        for (uint32_t current = state_index; states[current].parent != UINT32_MAX;
             current = states[current].parent) {
            chain.push_back(all_edges[states[current].edge].word);
        }
        Candidate candidate;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            candidate.text += (*it)->text;
            if (!candidate.syllables.empty()) candidate.syllables += ':';
            candidate.syllables += (*it)->syllables;
        }
        if (std::any_of(results.begin(), results.end(),
                        [&](const Candidate& r) { return r.text == candidate.text; })) {
            continue;
        }
        candidate.code = input;
        candidate.input_code = input;
        candidate.origin = CandidateOrigin::kComposed;
        candidate.source = CandidateSource::kPinyin;
        candidate.source_frequency = final_state.frequency;
        candidate.frequency = sentence_frequency(final_state.score);
        results.push_back(std::move(candidate));
    }
    return results;
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
    // A long input whose last syllable is still being typed ("...yizhengj") has no whole-input
    // candidate. Offer the sentence for the syllables typed so far first, as Microsoft Pinyin
    // does: taking it leaves the last letters to go on with.
    if (full_count == 0 && sentence_composition_enabled_ &&
        pinyin_scheme() == PinyinSchemeKind::kFullPinyin && !pinyin_query_policy_.initials_only &&
        request.input.size() >= kMinLeadingSentenceInput) {
        for (std::size_t cut = 1; cut <= kMaxUnfinishedLetters &&
                                  cut + kMinLeadingSentenceInput <= request.input.size() + 1;
             ++cut) {
            if (request.budget && request.budget->deadline.expired()) {
                break;
            }
            const std::string prefix = request.input.substr(0, request.input.size() - cut);
            CandidatePage page = translate_page(prefix, 0, kLeadingSentenceFetch, nullptr,
                                                request.budget, nullptr, 0, false);
            const auto sentence = std::find_if(
                page.candidates.begin(), page.candidates.end(), [&](const Candidate& candidate) {
                    return candidate_syllable_count(candidate) >= 2 &&
                           spells_input(prefix, candidate.syllables);
                });
            if (sentence != page.candidates.end()) {
                Candidate candidate = *sentence;
                candidate.source = CandidateSource::kPinyin;
                merged.insert(merged.begin(),
                              make_text_candidate_entry(std::move(candidate), prefix.size()));
                break;
            }
        }
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
