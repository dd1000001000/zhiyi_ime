// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/disabled_system_lexicon.h>

#include <algorithm>
#include <sstream>
#include <utility>

#include <cxxime/user_dict_validation.h>
#include <cxxime/user_data_merge.h>

#include "user_data_file.h"

namespace cxxime {
namespace {

bool is_valid_entry(const std::string& text) { return is_valid_user_dict_text(text); }

bool is_filterable_candidate(const Candidate& candidate) {
    return candidate.origin != CandidateOrigin::kUser &&
           candidate.source != CandidateSource::kSymbol &&
           candidate.source != CandidateSource::kPhrase;
}

} // namespace

bool DisabledSystemLexicon::load(const std::string& path) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::string contents;
    if (!read_user_data_file(path, &contents)) {
        return false;
    }

    std::unordered_set<std::string> entries;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (is_valid_entry(line)) {
            entries.insert(std::move(line));
        }
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_ = std::move(entries);
    path_ = path;
    entry_count_.store(entries_.size(), std::memory_order_release);
    dirty_.store(false, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool DisabledSystemLexicon::merge_contents_and_save(const std::string& imported,
                                                    UserDataMergeResult* result) {
    if (!result) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::unordered_set<std::string> current;
    std::string path;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        current = entries_;
        path = path_;
    }
    UserDataMergeResult merged;
    if (path.empty() ||
        !merge_lexicon_resource_contents(LexiconResource::kDisabledSystemLexicon, kind_,
                                         serialize_entries(current), imported, &merged)) {
        return false;
    }
    std::unordered_set<std::string> next;
    std::istringstream input(merged.contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            next.insert(std::move(line));
        }
    }
    if (!write_user_data_file_atomically(path, merged.contents)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_ = std::move(next);
    entry_count_.store(entries_.size(), std::memory_order_release);
    dirty_.store(false, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    *result = std::move(merged);
    return true;
}

bool DisabledSystemLexicon::validate_contents(const std::string& contents) {
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && !is_valid_entry(line)) {
            return false;
        }
    }
    return true;
}

bool DisabledSystemLexicon::save() {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
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

bool DisabledSystemLexicon::disable(const std::string& text) {
    if (!is_valid_entry(text)) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto inserted = entries_.insert(text);
    if (!inserted.second) {
        return true;
    }
    entry_count_.store(entries_.size(), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool DisabledSystemLexicon::restore(const std::string& text) {
    if (!is_valid_entry(text)) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (entries_.erase(text) == 0) {
        return true;
    }
    entry_count_.store(entries_.size(), std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

std::string
DisabledSystemLexicon::serialize_entries(const std::unordered_set<std::string>& entries) {
    std::vector<std::string> sorted(entries.begin(), entries.end());
    std::sort(sorted.begin(), sorted.end());
    std::ostringstream output;
    for (const auto& text : sorted) {
        output << text << '\n';
    }
    return output.str();
}

bool DisabledSystemLexicon::persist_entries(std::unordered_set<std::string> entries,
                                            const std::string& path) {
    if (path.empty() || !write_user_data_file_atomically(path, serialize_entries(entries))) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_ = std::move(entries);
    entry_count_.store(entries_.size(), std::memory_order_release);
    dirty_.store(false, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool DisabledSystemLexicon::disable_and_save(const std::string& text) {
    if (!is_valid_entry(text)) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::unordered_set<std::string> entries;
    std::string path;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (entries_.find(text) != entries_.end()) {
            return true;
        }
        entries = entries_;
        path = path_;
    }
    entries.insert(text);
    return persist_entries(std::move(entries), path);
}

bool DisabledSystemLexicon::restore_and_save(const std::string& text) {
    if (!is_valid_entry(text)) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::unordered_set<std::string> entries;
    std::string path;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (entries_.find(text) == entries_.end()) {
            return true;
        }
        entries = entries_;
        path = path_;
    }
    entries.erase(text);
    return persist_entries(std::move(entries), path);
}

bool DisabledSystemLexicon::contains(const std::string& text) const {
    if (empty()) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entries_.find(text) != entries_.end();
}

void DisabledSystemLexicon::filter(std::vector<Candidate>& candidates) const {
    if (empty()) {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const Candidate& candidate) {
                                        return is_filterable_candidate(candidate) &&
                                               entries_.find(candidate.text) != entries_.end();
                                    }),
                     candidates.end());
}

std::vector<UserDictEntryInfo> DisabledSystemLexicon::query(const std::string& query,
                                                            std::size_t offset, std::size_t limit,
                                                            std::size_t* match_total) const {
    std::vector<std::string> matches;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        matches.reserve(entries_.size());
        for (const auto& text : entries_) {
            if (query.empty() || text.find(query) != std::string::npos) {
                matches.push_back(text);
            }
        }
    }
    std::sort(matches.begin(), matches.end());
    if (match_total) {
        *match_total = matches.size();
    }
    if (offset >= matches.size() || limit == 0) {
        return {};
    }
    const std::size_t end = offset + (std::min)(matches.size() - offset, limit);
    std::vector<UserDictEntryInfo> result;
    result.reserve(end - offset);
    for (std::size_t index = offset; index < end; ++index) {
        UserDictEntryInfo entry;
        entry.text = std::move(matches[index]);
        result.push_back(std::move(entry));
    }
    return result;
}

bool DisabledSystemLexicon::empty() const {
    return entry_count_.load(std::memory_order_acquire) == 0;
}

std::size_t DisabledSystemLexicon::entry_count() const {
    return entry_count_.load(std::memory_order_acquire);
}

std::uint64_t DisabledSystemLexicon::version() const {
    return version_.load(std::memory_order_acquire);
}

} // namespace cxxime
