// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/candidate_preference.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <utility>

#include <windows.h>

#include <cxxime/input_limits.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>
#include <cxxime/user_data_merge.h>

#include "user_data_file.h"

namespace cxxime {
namespace {

constexpr int kPreferenceBaseScore = 210000000;

std::vector<std::string> split_tsv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string::npos) {
            return fields;
        }
        start = separator + 1;
    }
}

int parse_frequency(const std::string& value) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1) {
        return 1;
    }
    return parsed > INT_MAX ? INT_MAX : static_cast<int>(parsed);
}

std::uint64_t parse_sequence(const std::string& value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<std::uint64_t>(parsed) : 0;
}

void trim_trailing_space(std::string& value) {
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
}

int preference_score(int frequency, std::uint64_t current_sequence, std::uint64_t entry_sequence) {
    const std::uint64_t delta =
        current_sequence >= entry_sequence ? current_sequence - entry_sequence : 0;
    const int recency = delta <= 1000 ? static_cast<int>(1000 - delta) : 0;
    return kPreferenceBaseScore + (std::min)(frequency, 50000) + recency;
}

} // namespace

std::string CandidatePreference::entry_key(const std::string& text, const std::string& code) {
    std::string key;
    key.reserve(text.size() + code.size() + 1);
    key.append(code);
    key.push_back('\x1f');
    key.append(text);
    return key;
}

std::string CandidatePreference::serialize_entries(const std::vector<Entry>& entries) {
    std::ostringstream output;
    for (const auto& entry : entries) {
        if (entry.deleted) {
            continue;
        }
        output << entry.text << '\t' << entry.code << '\t' << entry.candidate_code << '\t'
               << entry.frequency << '\t' << entry.sequence << '\t' << entry.syllables << '\n';
    }
    return output.str();
}

bool CandidatePreference::validate_contents(const std::string& contents) {
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_tsv_line(line);
        char* frequency_end = nullptr;
        char* sequence_end = nullptr;
        errno = 0;
        const long frequency =
            fields.size() == 6 ? std::strtol(fields[3].c_str(), &frequency_end, 10) : 0;
        const int frequency_error = errno;
        errno = 0;
        const unsigned long long sequence =
            fields.size() == 6 ? std::strtoull(fields[4].c_str(), &sequence_end, 10) : 0;
        if (fields.size() != 6 || !is_valid_user_dict_text(fields[0]) ||
            !is_valid_user_dict_code(fields[1]) || !is_valid_user_dict_code(fields[2]) ||
            !is_valid_user_dict_syllables(fields[5]) || frequency_error != 0 || !frequency_end ||
            *frequency_end != '\0' || frequency < 1 || frequency > INT_MAX || errno != 0 ||
            !sequence_end || *sequence_end != '\0' || fields[4].empty() ||
            fields[4].front() == '-' || sequence == 0) {
            return false;
        }
    }
    return true;
}

bool CandidatePreference::load(const std::string& path) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::string contents;
    if (!read_user_data_file(path, &contents)) {
        return false;
    }

    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> fields = split_tsv_line(line);
        if (fields.size() != 6 || !is_ordinary_candidate_text(fields[0]) || fields[1].empty() ||
            fields[2].empty()) {
            continue;
        }
        Entry entry;
        entry.text = fields[0];
        entry.code = fields[1];
        entry.candidate_code = fields[2];
        entry.frequency = parse_frequency(fields[3]);
        entry.sequence = parse_sequence(fields[4]);
        entry.syllables = fields[5];
        trim_trailing_space(entry.syllables);
        sequence = (std::max)(sequence, entry.sequence);
        entries.push_back(std::move(entry));
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_ = std::move(entries);
    path_ = path;
    sequence_ = sequence;
    rebuild_indexes_locked();
    dirty_.store(false, std::memory_order_release);
    accepting_updates_ = true;
    last_update_ms_.store(0, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool CandidatePreference::merge_contents_and_save(const std::string& imported,
                                                  UserDataMergeResult* result) {
    if (!result) {
        return false;
    }
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const std::string current = serialize_entries(entries_);
    const std::string path = path_;
    UserDataMergeResult merged;
    if (path.empty() ||
        !merge_lexicon_resource_contents(LexiconResource::kCandidatePreference, kind_, current,
                                         imported, &merged)) {
        return false;
    }

    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    std::istringstream input(merged.contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> values = split_tsv_line(line);
        if (values.size() != 6) {
            return false;
        }
        Entry entry;
        entry.text = values[0];
        entry.code = values[1];
        entry.candidate_code = values[2];
        entry.frequency = parse_frequency(values[3]);
        entry.sequence = parse_sequence(values[4]);
        entry.syllables = values[5];
        trim_trailing_space(entry.syllables);
        sequence = (std::max)(sequence, entry.sequence);
        entries.push_back(std::move(entry));
    }
    if (!write_user_data_file_atomically(path, merged.contents)) {
        return false;
    }
    entries_ = std::move(entries);
    sequence_ = sequence;
    rebuild_indexes_locked();
    dirty_.store(false, std::memory_order_release);
    last_update_ms_.store(0, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    *result = std::move(merged);
    return true;
}

bool CandidatePreference::save() {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::string path;
    std::string contents;
    std::uint64_t saved_version = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!dirty_.load(std::memory_order_acquire) || path_.empty()) {
            return true;
        }
        path = path_;
        contents = serialize_entries(entries_);
        saved_version = version_.load(std::memory_order_acquire);
    }
    if (!write_user_data_file_atomically(path, contents)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (version_.load(std::memory_order_acquire) == saved_version) {
        dirty_.store(false, std::memory_order_release);
    }
    return true;
}

bool CandidatePreference::save_if_due(std::chrono::milliseconds delay) {
    if (!dirty_.load(std::memory_order_acquire)) {
        return true;
    }
    const std::uint64_t updated = last_update_ms_.load(std::memory_order_acquire);
    const std::uint64_t now = GetTickCount64();
    if (updated != 0 && now - updated < static_cast<std::uint64_t>(delay.count())) {
        return true;
    }
    return save();
}

void CandidatePreference::freeze() {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    accepting_updates_ = false;
}

bool CandidatePreference::record(const Candidate& candidate, const std::string& code) {
    if (candidate.text.empty() || code.empty() || candidate.source == CandidateSource::kSymbol ||
        candidate.source == CandidateSource::kEnglish ||
        candidate.origin == CandidateOrigin::kComposed || code.size() > kMaxInputCodeLength ||
        std::any_of(code.begin(), code.end(), [](char ch) { return ch < 'a' || ch > 'z'; })) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!accepting_updates_) {
        return false;
    }
    const std::string key = entry_key(candidate.text, code);
    const auto found = entry_index_.find(key);
    if (found != entry_index_.end()) {
        Entry& entry = entries_[found->second];
        if (entry.frequency < INT_MAX) {
            ++entry.frequency;
        }
        entry.sequence = ++sequence_;
        if (entry.syllables.empty()) {
            entry.syllables = candidate.syllables;
        }
        if (entry.candidate_code.empty()) {
            entry.candidate_code = candidate.code.empty() ? code : candidate.code;
        }
    } else {
        Entry entry;
        entry.text = candidate.text;
        entry.code = code;
        entry.candidate_code = candidate.code.empty() ? code : candidate.code;
        entry.syllables = candidate.syllables;
        entry.sequence = ++sequence_;
        const EntryId id = static_cast<EntryId>(entries_.size());
        entries_.push_back(std::move(entry));
        entry_index_[key] = id;
        code_index_[code].push_back(id);
    }
    last_update_ms_.store(GetTickCount64(), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

std::vector<Candidate> CandidatePreference::preferred_candidates(const std::string& code,
                                                                 CandidateSource source) const {
    std::vector<Candidate> candidates;
    if (code.empty()) {
        return candidates;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = code_index_.find(code);
    if (found == code_index_.end()) {
        return candidates;
    }

    candidates.reserve(found->second.size());
    for (EntryId id : found->second) {
        const Entry& entry = entries_[id];
        if (entry.deleted) {
            continue;
        }
        Candidate learned;
        learned.text = entry.text;
        learned.code = entry.candidate_code;
        learned.syllables = entry.syllables;
        learned.frequency = preference_score(entry.frequency, sequence_, entry.sequence);
        learned.source = source;
        learned.origin = CandidateOrigin::kLearned;
        candidates.push_back(std::move(learned));
    }
    return candidates;
}

std::vector<UserDictEntryInfo> CandidatePreference::query(const std::string& query,
                                                          std::size_t offset, std::size_t limit,
                                                          std::size_t* match_total) const {
    std::vector<UserDictEntryInfo> results;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.deleted || (!query.empty() && entry.text.find(query) == std::string::npos &&
                              entry.code.find(query) == std::string::npos)) {
            continue;
        }
        results.push_back(
            {entry.text, entry.code, entry.frequency, entry.sequence, entry.syllables});
    }
    std::sort(results.begin(), results.end(),
              [](const UserDictEntryInfo& left, const UserDictEntryInfo& right) {
                  if (left.sequence != right.sequence) {
                      return left.sequence > right.sequence;
                  }
                  if (left.frequency != right.frequency) {
                      return left.frequency > right.frequency;
                  }
                  if (left.code != right.code) {
                      return left.code < right.code;
                  }
                  return left.text < right.text;
              });
    if (match_total) {
        *match_total = results.size();
    }
    if (offset >= results.size() || limit == 0) {
        return {};
    }
    const std::size_t end = (std::min)(results.size(), offset + limit);
    return std::vector<UserDictEntryInfo>(results.begin() + offset, results.begin() + end);
}

bool CandidatePreference::erase(const std::vector<LexiconEntryKey>& entries) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!accepting_updates_ || entries.empty()) {
        return false;
    }

    bool changed = false;
    for (const auto& entry : entries) {
        const auto found = entry_index_.find(entry_key(entry.text, entry.code));
        if (found != entry_index_.end()) {
            entries_[found->second].deleted = true;
            changed = true;
        }
    }
    if (!changed) {
        return false;
    }
    rebuild_indexes_locked();
    last_update_ms_.store(GetTickCount64(), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool CandidatePreference::clear() {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!accepting_updates_) {
        return false;
    }
    if (entry_index_.empty()) {
        return true;
    }
    entries_.clear();
    entry_index_.clear();
    code_index_.clear();
    sequence_ = 0;
    last_update_ms_.store(GetTickCount64(), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool CandidatePreference::erase_and_save(const std::vector<LexiconEntryKey>& requested_entries) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::vector<Entry> entries;
    std::string path;
    std::uint64_t captured_version = 0;
    std::unordered_map<std::string, std::uint64_t> target_sequences;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!accepting_updates_ || requested_entries.empty()) {
            return false;
        }

        entries = entries_;
        target_sequences.reserve(requested_entries.size());
        for (const auto& requested : requested_entries) {
            const std::string key = entry_key(requested.text, requested.code);
            const auto found = entry_index_.find(key);
            if (found == entry_index_.end()) {
                continue;
            }
            entries[found->second].deleted = true;
            target_sequences.emplace(key, entries_[found->second].sequence);
        }
        if (target_sequences.empty()) {
            return false;
        }
        path = path_;
        captured_version = version_.load(std::memory_order_acquire);
    }
    if (path.empty() || !write_user_data_file_atomically(path, serialize_entries(entries))) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    const bool concurrent_update = version_.load(std::memory_order_acquire) != captured_version;
    bool changed = false;
    for (const auto& target : target_sequences) {
        const auto found = entry_index_.find(target.first);
        if (found != entry_index_.end() && entries_[found->second].sequence == target.second) {
            entries_[found->second].deleted = true;
            changed = true;
        }
    }
    if (changed) {
        rebuild_indexes_locked();
        version_.fetch_add(1, std::memory_order_acq_rel);
    }
    dirty_.store(concurrent_update, std::memory_order_release);
    if (!concurrent_update) {
        last_update_ms_.store(0, std::memory_order_release);
    }
    return true;
}

bool CandidatePreference::erase_code_and_save(const std::string& code) {
    std::vector<LexiconEntryKey> entries;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto found = code_index_.find(code);
        if (found == code_index_.end()) {
            return true;
        }
        entries.reserve(found->second.size());
        for (EntryId id : found->second) {
            entries.push_back({entries_[id].text, entries_[id].code});
        }
    }
    return erase_and_save(entries);
}

bool CandidatePreference::clear_and_save() {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    std::string path;
    std::uint64_t captured_sequence = 0;
    std::uint64_t captured_version = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!accepting_updates_) {
            return false;
        }
        if (entry_index_.empty() && !dirty_.load(std::memory_order_acquire)) {
            return true;
        }
        path = path_;
        captured_sequence = sequence_;
        captured_version = version_.load(std::memory_order_acquire);
    }
    if (path.empty() || !write_user_data_file_atomically(path, {})) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    const bool concurrent_update = version_.load(std::memory_order_acquire) != captured_version;
    bool changed = false;
    for (auto& entry : entries_) {
        if (!entry.deleted && entry.sequence <= captured_sequence) {
            entry.deleted = true;
            changed = true;
        }
    }
    if (changed) {
        rebuild_indexes_locked();
        version_.fetch_add(1, std::memory_order_acq_rel);
    }
    dirty_.store(concurrent_update, std::memory_order_release);
    if (!concurrent_update) {
        last_update_ms_.store(0, std::memory_order_release);
    }
    return true;
}

bool CandidatePreference::contains(const std::string& text, const std::string& code) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entry_index_.find(entry_key(text, code)) != entry_index_.end();
}

std::size_t CandidatePreference::entry_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entry_index_.size();
}

std::uint64_t CandidatePreference::version() const {
    return version_.load(std::memory_order_acquire);
}

bool CandidatePreference::dirty() const { return dirty_.load(std::memory_order_acquire); }

void CandidatePreference::rebuild_indexes_locked() {
    entry_index_.clear();
    code_index_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& entry = entries_[i];
        if (entry.deleted) {
            continue;
        }
        const EntryId id = static_cast<EntryId>(i);
        const std::string key = entry_key(entry.text, entry.code);
        const auto duplicate = entry_index_.find(key);
        if (duplicate != entry_index_.end()) {
            Entry& existing = entries_[duplicate->second];
            existing.frequency = (std::max)(existing.frequency, entry.frequency);
            existing.sequence = (std::max)(existing.sequence, entry.sequence);
            if (existing.syllables.empty()) {
                existing.syllables = entry.syllables;
            }
            entry.deleted = true;
            continue;
        }
        entry_index_[key] = id;
        code_index_[entry.code].push_back(id);
    }
    for (auto& item : code_index_) {
        std::sort(item.second.begin(), item.second.end(), [this](EntryId left, EntryId right) {
            return entries_[left].sequence > entries_[right].sequence;
        });
    }
}

} // namespace cxxime
