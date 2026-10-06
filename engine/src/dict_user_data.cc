// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/dict.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string_view>

#include <windows.h>
#include <shlobj.h>

#include <cxxime/candidate_preference.h>
#include <cxxime/disabled_system_lexicon.h>
#include <cxxime/manual_candidate_order.h>
#include <cxxime/user_lexicon.h>

#include "binary_format.h"

namespace cxxime {
namespace {

constexpr int kFallbackCandidateScore = 1;

std::string default_user_dict_path(UserDictKind kind) {
    wchar_t profile[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) != S_OK) {
        return {};
    }
    const std::wstring user_dir = std::wstring(profile) + L"\\zhiyi";
    CreateDirectoryW(user_dir.c_str(), nullptr);
    const std::wstring path =
        user_dir + (kind == UserDictKind::WUBI ? L"\\user_wubi.tsv" : L"\\user_pinyin.tsv");
    char path_utf8[MAX_PATH * 3] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, path_utf8,
                             static_cast<int>(sizeof(path_utf8)), nullptr, nullptr)) {
        return {};
    }
    return path_utf8;
}

} // namespace

bool Dict::load_user_dict(const std::string& path) {
    return user_lexicon_->load(path.empty() ? default_user_dict_path(kind_) : path);
}

bool Dict::save_user_dict() { return user_lexicon_->save(); }

bool Dict::add_user_entry(const std::string& text, const std::string& code,
                          const std::string& syllables) {
    return user_lexicon_->add_entry(text, code, syllables);
}

std::vector<UserDictEntryInfo> Dict::query_user_entries(const std::string& query, size_t offset,
                                                        size_t limit, size_t* match_total,
                                                        bool exact_text) const {
    return user_lexicon_->query_entries(query, offset, limit, match_total, exact_text);
}

bool Dict::delete_user_entries(const std::vector<LexiconEntryKey>& entries) {
    return user_lexicon_->delete_entries(entries);
}

bool Dict::replace_user_entry(const std::string& old_text, const std::string& old_code,
                              const std::string& new_text, const std::string& new_code,
                              const std::string& syllables) {
    return user_lexicon_->replace_entry(old_text, old_code, new_text, new_code, syllables);
}

bool Dict::add_user_entry_and_save(const std::string& text, const std::string& code,
                                   const std::string& syllables) {
    return user_lexicon_->add_entry_and_save(text, code, syllables);
}

bool Dict::delete_user_entries_and_save(const std::vector<LexiconEntryKey>& entries) {
    return user_lexicon_->delete_entries_and_save(entries);
}

bool Dict::replace_user_entry_and_save(const std::string& old_text, const std::string& old_code,
                                       const std::string& new_text, const std::string& new_code,
                                       const std::string& syllables) {
    return user_lexicon_->replace_entry_and_save(old_text, old_code, new_text, new_code,
                                                 syllables);
}

bool Dict::import_user_dict(const std::string& source_path) {
    return user_lexicon_->import_file(source_path);
}

bool Dict::merge_user_dict_contents(const std::string& imported, UserDataMergeResult* result) {
    return user_lexicon_->merge_contents_and_save(imported, result);
}

std::string Dict::reverse_lookup(const std::string& text) {
    std::string code = user_lexicon_->reverse_lookup(text);
    if (!code.empty()) {
        return code;
    }
    if (!dict_entries_) {
        return {};
    }
    for (uint32_t i = 0; i < dict_entry_count_; ++i) {
        const auto& entry = dict_entries_[i];
        if (entry.text_len == text.size() &&
            std::memcmp(dict_strings_ + entry.text_offset, text.data(), entry.text_len) == 0) {
            return std::string(dict_strings_ + entry.syllable_ids_offset, entry.syllable_ids_len);
        }
    }
    return {};
}

bool Dict::has_user_entry(const std::string& text) const {
    return user_lexicon_->contains_text(text);
}

size_t Dict::user_entry_count() const { return user_lexicon_->entry_count(); }

uint64_t Dict::user_dict_version() const { return user_lexicon_->version(); }

std::vector<Candidate> Dict::lookup_user_exact(const std::string& code, int limit,
                                               const QueryBudget& budget, QueryTrace* trace,
                                               UserLookupStats* stats) const {
    return user_lexicon_->lookup_exact(code, limit, budget, trace, stats);
}

std::vector<Candidate> Dict::lookup_user_prefix(const std::string& prefix, int limit,
                                                const QueryBudget& budget, QueryTrace* trace,
                                                UserLookupStats* stats) const {
    return user_lexicon_->lookup_prefix(prefix, limit, budget, trace, stats);
}

std::vector<Candidate> Dict::lookup_user_indexed(const std::string& key, int limit,
                                                 const QueryBudget& budget, QueryTrace* trace,
                                                 UserLookupStats* stats) const {
    return user_lexicon_->lookup_indexed(key, limit, budget, trace, stats);
}

bool Dict::load_candidate_preferences(const std::string& path) {
    return candidate_preference_->load(path);
}

bool Dict::save_candidate_preferences() { return candidate_preference_->save(); }

bool Dict::merge_candidate_preference_contents(const std::string& imported,
                                                UserDataMergeResult* result) {
    return candidate_preference_->merge_contents_and_save(imported, result);
}

bool Dict::save_candidate_preferences_if_due(std::chrono::milliseconds delay) {
    return candidate_preference_->save_if_due(delay);
}

void Dict::freeze_candidate_preferences() { candidate_preference_->freeze(); }

bool Dict::record_candidate_preference(const Candidate& candidate, const std::string& code,
                                       CandidatePreferenceReceipt* receipt) {
    return candidate_preference_->record(candidate, code, receipt);
}

bool Dict::revoke_candidate_preference(const CandidatePreferenceReceipt& receipt) {
    return candidate_preference_->revoke(receipt);
}

namespace {

// A tentative (picked once) word rises within its ranking group, as x20 frequency.
int boosted_score(int frequency) {
    if (frequency >= kRankingGroupsBegin && frequency < kRankingGroupsEnd) {
        const int group_top =
            (frequency / kRankingGroupSpan) * kRankingGroupSpan + kRankingGroupSpan - 1;
        return (std::min)(frequency + kLearnedBoost, group_top);
    }
    return frequency + kLearnedBoost;
}

}  // namespace

void Dict::apply_candidate_preferences(const std::string& code, CandidateSource source,
                                       std::vector<Candidate>& candidates, int limit) const {
    if (limit <= 0) {
        return;
    }
    auto preferences = candidate_preference_->preferred_candidates(code, source);
    const bool filter_disabled = disabled_system_entry_count() != 0;
    for (auto& preference : preferences) {
        if (filter_disabled && is_system_entry_disabled(preference.text) &&
            preference.origin != CandidateOrigin::kUser) {
            continue;
        }
        auto existing =
            std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
                return candidate.text == preference.text;
            });
        const bool boost_only = preference.frequency == kLearnedBoostRequest;
        if (existing != candidates.end()) {
            existing->learned = true;
            if (existing->origin == CandidateOrigin::kLearned) {
                continue;
            }
            existing->frequency = boost_only
                                      ? boosted_score(existing->frequency)
                                      : (std::max)(existing->frequency, preference.frequency);
            continue;
        }
        if (!preference_candidate_available(preference, source)) {
            preference.frequency = kFallbackCandidateScore;
        } else if (boost_only) {
            // Outside the fetched window: boosted from the score it had when picked.
            preference.frequency = preference.source_frequency > 0
                                       ? boosted_score(preference.source_frequency)
                                       : kLearnedAbsentScore;
        }
        preference.source_frequency = 0;
        candidates.push_back(std::move(preference));
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& left, const Candidate& right) {
                         return left.frequency > right.frequency;
                     });
    if (static_cast<int>(candidates.size()) > limit) {
        candidates.resize(static_cast<std::size_t>(limit));
    }
}

bool Dict::preference_candidate_available(const Candidate& candidate,
                                          CandidateSource source) const {
    if (user_lexicon_->contains_candidate(candidate.text, candidate.code, candidate.syllables)) {
        return true;
    }
    if (!dict_entries_) {
        return false;
    }

    if (candidate.syllables.empty() && source == CandidateSource::kPinyin &&
        short_cache_.is_loaded()) {
        const auto cached = short_cache_.lookup(candidate.code, 128);
        if (std::any_of(cached.begin(), cached.end(),
                        [&](const Candidate& item) { return item.text == candidate.text; })) {
            return true;
        }
    }

    const std::string_view target_code = candidate.syllables.empty()
        ? std::string_view(candidate.code)
        : std::string_view(candidate.syllables);
    auto entry_code = [&](std::uint32_t index) {
        const auto& entry = dict_entries_[index];
        return std::string_view(dict_strings_ + entry.syllable_ids_offset, entry.syllable_ids_len);
    };
    std::uint32_t lower = 0;
    std::uint32_t upper = dict_entry_count_;
    while (lower < upper) {
        const std::uint32_t middle = lower + (upper - lower) / 2;
        if (entry_code(middle) < target_code) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    for (std::uint32_t index = lower; index < dict_entry_count_ && entry_code(index) == target_code;
         ++index) {
        const auto& entry = dict_entries_[index];
        if (entry.text_len == candidate.text.size() &&
            std::memcmp(dict_strings_ + entry.text_offset, candidate.text.data(), entry.text_len) ==
                0) {
            return true;
        }
    }
    return false;
}

std::vector<UserDictEntryInfo> Dict::query_candidate_preferences(const std::string& query,
                                                                 size_t offset, size_t limit,
                                                                 size_t* match_total) const {
    return candidate_preference_->query(query, offset, limit, match_total);
}

bool Dict::delete_candidate_preferences(const std::vector<LexiconEntryKey>& entries) {
    return candidate_preference_->erase(entries);
}

bool Dict::clear_candidate_preferences() { return candidate_preference_->clear(); }

bool Dict::delete_candidate_preferences_and_save(const std::vector<LexiconEntryKey>& entries) {
    return candidate_preference_->erase_and_save(entries);
}

bool Dict::clear_candidate_preferences_and_save() {
    return candidate_preference_->clear_and_save();
}

bool Dict::clear_candidate_preferences_for_code_and_save(const std::string& code) {
    return candidate_preference_->erase_code_and_save(code);
}

size_t Dict::candidate_preference_count() const { return candidate_preference_->entry_count(); }

uint64_t Dict::candidate_preference_version() const { return candidate_preference_->version(); }

bool Dict::load_manual_candidate_order(const std::string& path) {
    return manual_candidate_order_->load(path);
}

bool Dict::merge_manual_candidate_order_contents(const std::string& imported,
                                                  UserDataMergeResult* result) {
    return manual_candidate_order_->merge_contents_and_save(imported, result);
}

bool Dict::resolve_manual_candidate(const ManualCandidateOrderEntry& entry, CandidateSource source,
                                    Candidate* candidate) const {
    if (!candidate) {
        return false;
    }
    if (user_lexicon_->contains_candidate_identity(entry.text, entry.code, entry.syllables)) {
        candidate->text = entry.text;
        candidate->code = entry.code;
        candidate->syllables = entry.syllables;
        candidate->frequency = kFallbackCandidateScore;
        candidate->source = source;
        candidate->origin = CandidateOrigin::kUser;
        return true;
    }
    if (!dict_entries_) {
        return false;
    }

    const std::string_view target_code =
        entry.syllables.empty() ? std::string_view(entry.code) : std::string_view(entry.syllables);
    auto entry_code = [&](std::uint32_t index) {
        const auto& dictionary_entry = dict_entries_[index];
        return std::string_view(dict_strings_ + dictionary_entry.syllable_ids_offset,
                                dictionary_entry.syllable_ids_len);
    };
    std::uint32_t lower = 0;
    std::uint32_t upper = dict_entry_count_;
    while (lower < upper) {
        const std::uint32_t middle = lower + (upper - lower) / 2;
        if (entry_code(middle) < target_code) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    for (std::uint32_t index = lower;
         index < dict_entry_count_ && entry_code(index) == target_code; ++index) {
        const auto& dictionary_entry = dict_entries_[index];
        if (dictionary_entry.text_len != entry.text.size() ||
            std::memcmp(dict_strings_ + dictionary_entry.text_offset, entry.text.data(),
                        dictionary_entry.text_len) != 0) {
            continue;
        }
        fill_system_candidate(index, *candidate, 0);
        if (candidate->code != entry.code || candidate->syllables != entry.syllables) {
            continue;
        }
        candidate->source = source;
        return true;
    }
    return false;
}

void Dict::apply_manual_candidate_order(const std::string& code, CandidateSource source,
                                        std::vector<Candidate>& candidates, int limit,
                                        std::string_view candidate_input_code) const {
    if (limit <= 0) {
        return;
    }
    const auto ordered = manual_candidate_order_->entries_for(code);
    if (ordered.empty()) {
        return;
    }

    std::vector<Candidate> fixed;
    fixed.reserve(ordered.size());
    for (const auto& entry : ordered) {
        auto existing = std::find_if(candidates.begin(), candidates.end(), [&](const auto& item) {
            return item.text == entry.text && item.code == entry.code &&
                   item.syllables == entry.syllables;
        });
        Candidate resolved;
        if (existing != candidates.end()) {
            resolved = std::move(*existing);
            candidates.erase(existing);
        } else if (!resolve_manual_candidate(entry, source, &resolved)) {
            continue;
        }
        if (!candidate_input_code.empty()) {
            resolved.input_code.assign(candidate_input_code.data(), candidate_input_code.size());
        }
        if (is_system_entry_disabled(resolved.text) && resolved.origin != CandidateOrigin::kUser) {
            continue;
        }
        const auto same_identity = [&](const auto& item) {
            return item.text == resolved.text && item.code == resolved.code &&
                   item.syllables == resolved.syllables;
        };
        if (std::none_of(fixed.begin(), fixed.end(), same_identity)) {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), same_identity),
                             candidates.end());
            fixed.push_back(std::move(resolved));
        }
    }
    candidates.insert(candidates.begin(), std::make_move_iterator(fixed.begin()),
                      std::make_move_iterator(fixed.end()));
    if (static_cast<int>(candidates.size()) > limit) {
        candidates.resize(static_cast<std::size_t>(limit));
    }
}

std::vector<ManualCandidateOrderEntry> Dict::manual_candidate_order(const std::string& code) const {
    return manual_candidate_order_->entries_for(code);
}

bool Dict::replace_manual_candidate_order_and_save(
    const std::string& code, const std::vector<ManualCandidateOrderEntry>& entries) {
    return manual_candidate_order_->replace_and_save(code, entries);
}

bool Dict::replace_manual_candidate_order_if_version(
    const std::string& code, const std::vector<ManualCandidateOrderEntry>& entries,
    uint64_t expected_version, bool* version_conflict) {
    return manual_candidate_order_->replace_and_save_if_version(code, entries, expected_version,
                                                                version_conflict);
}

bool Dict::has_manual_candidate_order(const std::string& input_code, const std::string& text,
                                      const std::string& candidate_code,
                                      const std::string& syllables) const {
    return manual_candidate_order_->contains(input_code, text, candidate_code, syllables);
}

bool Dict::has_candidate_preference(const std::string& text, const std::string& code) const {
    return candidate_preference_->contains(text, code);
}

bool Dict::can_resolve_manual_candidate(const ManualCandidateOrderEntry& entry,
                                        CandidateSource source) const {
    Candidate candidate;
    return resolve_manual_candidate(entry, source, &candidate) &&
           (candidate.origin == CandidateOrigin::kUser ||
            !is_system_entry_disabled(candidate.text));
}

uint64_t Dict::manual_candidate_order_version() const {
    return manual_candidate_order_->version();
}

bool Dict::load_disabled_system_entries(const std::string& path) {
    return disabled_system_lexicon_->load(path);
}

bool Dict::merge_disabled_system_contents(const std::string& imported,
                                          UserDataMergeResult* result) {
    return disabled_system_lexicon_->merge_contents_and_save(imported, result);
}

bool Dict::save_disabled_system_entries() { return disabled_system_lexicon_->save(); }

bool Dict::disable_system_entry(const std::string& text) {
    return disabled_system_lexicon_->disable(text);
}

bool Dict::restore_system_entry(const std::string& text) {
    return disabled_system_lexicon_->restore(text);
}

bool Dict::disable_system_entry_and_save(const std::string& text) {
    return disabled_system_lexicon_->disable_and_save(text);
}

bool Dict::restore_system_entry_and_save(const std::string& text) {
    return disabled_system_lexicon_->restore_and_save(text);
}

bool Dict::is_system_entry_disabled(const std::string& text) const {
    return disabled_system_lexicon_->contains(text);
}

void Dict::filter_disabled_system_candidates(std::vector<Candidate>& candidates) const {
    disabled_system_lexicon_->filter(candidates);
}

std::vector<UserDictEntryInfo> Dict::query_disabled_system_entries(
    const std::string& query, size_t offset, size_t limit, size_t* match_total) const {
    return disabled_system_lexicon_->query(query, offset, limit, match_total);
}

size_t Dict::disabled_system_entry_count() const {
    return disabled_system_lexicon_->entry_count();
}

uint64_t Dict::disabled_system_entry_version() const {
    return disabled_system_lexicon_->version();
}

} // namespace cxxime
