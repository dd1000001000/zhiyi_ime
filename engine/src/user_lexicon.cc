// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/user_lexicon.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <cxxime/ordinary_candidate.h>
#include <cxxime/pinyin_user_code.h>
#include <cxxime/user_dict_validation.h>

#include "user_data_file.h"

namespace cxxime {
namespace {

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

bool parse_frequency(const std::string& value, int* frequency) {
    if (!frequency || value.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1) {
        return false;
    }
    *frequency = parsed > INT_MAX ? INT_MAX : static_cast<int>(parsed);
    return true;
}

void trim_trailing_space(std::string& value) {
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
}

} // namespace

UserLexicon::UserLexicon(UserDictKind kind) : kind_(kind) {}

std::string UserLexicon::entry_key(const std::string& text, const std::string& code) {
    std::string key;
    key.reserve(text.size() + code.size() + 1);
    key.append(code);
    key.push_back('\x1f');
    key.append(text);
    return key;
}

bool UserLexicon::parse_entries(const std::string& contents, bool reject_invalid_lines,
                                UserDictKind kind, bool require_canonical_pinyin,
                                std::vector<Entry>* entries, std::uint64_t* sequence) {
    if (!entries || !sequence) {
        return false;
    }
    entries->clear();
    *sequence = 0;
    std::unordered_map<std::string, std::size_t> entry_positions;
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
        int frequency = 1;
        std::string syllables;
        bool valid = fields.size() == 3 || fields.size() == 4;
        if (valid) {
            syllables = fields.size() == 4 ? fields[3] : std::string{};
            trim_trailing_space(syllables);
            valid = parse_frequency(fields[2], &frequency) &&
                    is_valid_user_dict_entry(fields[0], fields[1], syllables);
            if (valid && require_canonical_pinyin && kind == UserDictKind::PINYIN) {
                valid = is_canonical_pinyin_user_code(fields[1], syllables);
            }
        }
        if (!valid) {
            if (reject_invalid_lines) {
                entries->clear();
                *sequence = 0;
                return false;
            }
            continue;
        }

        if (!is_ordinary_candidate_text(fields[0])) {
            continue;
        }
        Entry entry;
        entry.text = fields[0];
        entry.code = fields[1];
        entry.frequency = frequency;
        entry.syllables = std::move(syllables);
        entry.sequence = ++(*sequence);
        const std::string key = entry_key(entry.text, entry.code);
        const auto duplicate = entry_positions.find(key);
        if (duplicate != entry_positions.end()) {
            Entry& existing = (*entries)[duplicate->second];
            existing.frequency = (std::max)(existing.frequency, entry.frequency);
            continue;
        }
        entry_positions.emplace(key, entries->size());
        entries->push_back(std::move(entry));
    }
    return true;
}

bool UserLexicon::validate_contents(const std::string& contents, UserDictKind kind) {
    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    return parse_entries(contents, true, kind, true, &entries, &sequence);
}

std::string UserLexicon::serialize_entries(const std::vector<Entry>& entries) {
    std::ostringstream output;
    for (const auto& entry : entries) {
        if (entry.deleted) {
            continue;
        }
        output << entry.text << '\t' << entry.code << '\t' << entry.frequency;
        if (!entry.syllables.empty()) {
            output << '\t' << entry.syllables;
        }
        output << '\n';
    }
    return output.str();
}

bool UserLexicon::add_to_snapshot(Snapshot* snapshot, const std::string& text,
                                  const std::string& code,
                                  const std::string& syllables) const {
    if (!snapshot || !is_valid_user_dict_entry(text, code, syllables) ||
        !is_ordinary_candidate_text(text) ||
        (kind_ == UserDictKind::PINYIN &&
         !is_canonical_pinyin_user_code(code, syllables))) {
        return false;
    }
    const std::string key = entry_key(text, code);
    if (std::any_of(snapshot->entries.begin(), snapshot->entries.end(), [&](const Entry& entry) {
            return !entry.deleted && entry_key(entry.text, entry.code) == key;
        })) {
        return false;
    }
    Entry entry;
    entry.text = text;
    entry.code = code;
    entry.syllables = syllables;
    entry.sequence = ++snapshot->sequence;
    snapshot->entries.push_back(std::move(entry));
    return true;
}

bool UserLexicon::delete_from_snapshot(Snapshot* snapshot,
                                       const std::vector<LexiconEntryKey>& entries) {
    if (!snapshot || entries.empty()) {
        return false;
    }

    std::unordered_set<std::string> targets;
    targets.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!is_valid_user_dict_entry(entry.text, entry.code)) {
            return false;
        }
        targets.insert(entry_key(entry.text, entry.code));
    }

    bool changed = false;
    for (auto& entry : snapshot->entries) {
        if (!entry.deleted && targets.find(entry_key(entry.text, entry.code)) != targets.end()) {
            entry.deleted = true;
            changed = true;
        }
    }
    return changed;
}

bool UserLexicon::replace_in_snapshot(Snapshot* snapshot, const std::string& old_text,
                                      const std::string& old_code, const std::string& new_text,
                                      const std::string& new_code,
                                      const std::string& syllables) const {
    if (!snapshot || !is_valid_user_dict_entry(new_text, new_code, syllables) ||
        !is_ordinary_candidate_text(new_text) ||
        (kind_ == UserDictKind::PINYIN &&
         !is_canonical_pinyin_user_code(new_code, syllables))) {
        return false;
    }
    Entry* target = nullptr;
    for (auto& entry : snapshot->entries) {
        if (entry.deleted) {
            continue;
        }
        if (entry.text == old_text && entry.code == old_code) {
            target = &entry;
        } else if (entry.text == new_text && entry.code == new_code) {
            return false;
        }
    }
    if (!target) {
        return false;
    }
    target->text = new_text;
    const bool preserve_syllables = target->code == new_code && syllables.empty();
    target->code = new_code;
    if (!preserve_syllables) {
        target->syllables = syllables;
    }
    target->sequence = ++snapshot->sequence;
    target->abbr_code.clear();
    target->mixed_keys.clear();
    return true;
}

bool UserLexicon::load(const std::string& path) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::string contents;
    if (!read_user_data_file(path, &contents)) {
        return false;
    }

    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    if (!parse_entries(contents, false, kind_, false, &entries, &sequence)) {
        return false;
    }
    if (kind_ == UserDictKind::PINYIN) {
        const bool requires_migration = !validate_contents(contents, kind_);
        std::vector<Entry> canonical_entries;
        canonical_entries.reserve(entries.size());
        for (auto& entry : entries) {
            if (is_canonical_pinyin_user_code(entry.code, entry.syllables)) {
                canonical_entries.push_back(std::move(entry));
            }
        }
        if (requires_migration) {
            const std::string quarantine_path = path + ".noncanonical";
            std::string quarantine_contents;
            if (!read_user_data_file(quarantine_path, &quarantine_contents)) {
                return false;
            }
            const bool already_archived =
                quarantine_contents.size() >= contents.size() &&
                quarantine_contents.compare(quarantine_contents.size() - contents.size(),
                                            contents.size(), contents) == 0;
            if (!already_archived) {
                if (!quarantine_contents.empty() && quarantine_contents.back() != '\n') {
                    quarantine_contents.push_back('\n');
                }
                quarantine_contents.append(contents);
            }
            if (!write_user_data_file_atomically(quarantine_path, quarantine_contents) ||
                !write_user_data_file_atomically(path, serialize_entries(canonical_entries))) {
                return false;
            }
        }
        entries = std::move(canonical_entries);
    }

    Snapshot next;
    next.entries = std::move(entries);
    next.sequence = sequence;
    next.path = path;
    publish_snapshot(prepare_snapshot(std::move(next)), false);
    return true;
}

bool UserLexicon::save() {
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

bool UserLexicon::add_entry(const std::string& text, const std::string& code,
                            const std::string& syllables) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    if (!add_to_snapshot(&next, text, code, syllables)) {
        return false;
    }
    publish_snapshot(prepare_snapshot(std::move(next)), true);
    return true;
}

bool UserLexicon::delete_entries(const std::vector<LexiconEntryKey>& entries) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    if (!delete_from_snapshot(&next, entries)) {
        return false;
    }
    publish_snapshot(prepare_snapshot(std::move(next)), true);
    return true;
}

bool UserLexicon::replace_entry(const std::string& old_text, const std::string& old_code,
                                const std::string& new_text, const std::string& new_code,
                                const std::string& syllables) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    if (!replace_in_snapshot(&next, old_text, old_code, new_text, new_code, syllables)) {
        return false;
    }
    publish_snapshot(prepare_snapshot(std::move(next)), true);
    return true;
}

UserLexicon::Snapshot UserLexicon::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Snapshot result;
    result.entries = entries_;
    result.sequence = sequence_;
    result.path = path_;
    return result;
}

void UserLexicon::publish_snapshot(Snapshot snapshot, bool dirty) {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        entries_.swap(snapshot.entries);
        text_index_.swap(snapshot.text_index);
        entry_index_.swap(snapshot.entry_index);
        exact_index_.swap(snapshot.exact_index);
        prefix_index_.swap(snapshot.prefix_index);
        abbr_index_.swap(snapshot.abbr_index);
        mixed_index_.swap(snapshot.mixed_index);
        code_sorted_.swap(snapshot.code_sorted);
        sequence_ = snapshot.sequence;
        path_.swap(snapshot.path);
        dirty_.store(dirty, std::memory_order_release);
        version_.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool UserLexicon::persist_snapshot(Snapshot snapshot) {
    snapshot = prepare_snapshot(std::move(snapshot));
    if (snapshot.path.empty() ||
        !write_user_data_file_atomically(snapshot.path, serialize_entries(snapshot.entries))) {
        return false;
    }
    publish_snapshot(std::move(snapshot), false);
    return true;
}

bool UserLexicon::add_entry_and_save(const std::string& text, const std::string& code,
                                     const std::string& syllables) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    if (!is_valid_user_dict_entry(text, code, syllables) ||
        !is_ordinary_candidate_text(text) ||
        (kind_ == UserDictKind::PINYIN &&
         !is_canonical_pinyin_user_code(code, syllables))) {
        return false;
    }
    const std::string key = entry_key(text, code);
    const bool exists =
        std::any_of(next.entries.begin(), next.entries.end(), [&](const Entry& entry) {
            return !entry.deleted && entry_key(entry.text, entry.code) == key;
        });
    if (exists) {
        return true;
    }
    return add_to_snapshot(&next, text, code, syllables) && persist_snapshot(std::move(next));
}

bool UserLexicon::delete_entries_and_save(const std::vector<LexiconEntryKey>& entries) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    return delete_from_snapshot(&next, entries) && persist_snapshot(std::move(next));
}

bool UserLexicon::replace_entry_and_save(const std::string& old_text, const std::string& old_code,
                                         const std::string& new_text, const std::string& new_code,
                                         const std::string& syllables) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot next = snapshot();
    return replace_in_snapshot(&next, old_text, old_code, new_text, new_code, syllables) &&
           persist_snapshot(std::move(next));
}

bool UserLexicon::import_file(const std::string& source_path) {
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    std::string contents;
    if (!read_existing_user_data_file(source_path, kMaxUserDictImportBytes, &contents)) {
        return false;
    }

    std::vector<Entry> imported_entries;
    std::uint64_t imported_sequence = 0;
    if (!parse_entries(contents, true, kind_, true, &imported_entries, &imported_sequence)) {
        return false;
    }
    Snapshot next;
    next.entries = std::move(imported_entries);
    next.sequence = imported_sequence;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        next.path = path_;
    }
    next = prepare_snapshot(std::move(next));
    if (next.path.empty() ||
        !write_user_data_file_atomically(next.path, serialize_entries(next.entries))) {
        return false;
    }
    publish_snapshot(std::move(next), false);
    return true;
}

bool UserLexicon::merge_contents_and_save(const std::string& imported,
                                          UserDataMergeResult* result) {
    if (!result) {
        return false;
    }
    std::lock_guard<std::mutex> transaction_lock(transaction_mutex_);
    Snapshot current = snapshot();
    UserDataMergeResult merged;
    if (current.path.empty() ||
        !merge_lexicon_resource_contents(LexiconResource::kUserLexicon, kind_,
                                         serialize_entries(current.entries), imported, &merged)) {
        return false;
    }
    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    if (!parse_entries(merged.contents, true, kind_, true, &entries, &sequence)) {
        return false;
    }
    current.entries = std::move(entries);
    current.sequence = sequence;
    current = prepare_snapshot(std::move(current));
    if (!write_user_data_file_atomically(current.path, serialize_entries(current.entries))) {
        return false;
    }
    publish_snapshot(std::move(current), false);
    *result = std::move(merged);
    return true;
}

std::vector<UserDictEntryInfo> UserLexicon::query_entries(const std::string& query,
                                                          std::size_t offset, std::size_t limit,
                                                          std::size_t* match_total,
                                                          bool exact_text) const {
    std::vector<UserDictEntryInfo> results;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        const bool matches = exact_text
                                 ? !query.empty() && entry.text == query
                                 : query.empty() || entry.text.find(query) != std::string::npos ||
                                       entry.code.find(query) != std::string::npos;
        if (entry.deleted || !matches) {
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

std::string UserLexicon::reverse_lookup(const std::string& text) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = text_index_.find(text);
    if (found == text_index_.end()) {
        return {};
    }
    for (EntryId id : found->second) {
        if (id < entries_.size() && !entries_[id].deleted) {
            return entries_[id].code;
        }
    }
    return {};
}

bool UserLexicon::contains_text(const std::string& text) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = text_index_.find(text);
    return found != text_index_.end() && !found->second.empty();
}

bool UserLexicon::contains_candidate(const std::string& text, const std::string& code,
                                     const std::string& syllables) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = text_index_.find(text);
    if (found == text_index_.end()) {
        return false;
    }
    for (EntryId id : found->second) {
        const Entry& entry = entries_[id];
        if (!entry.deleted &&
            (entry.code == code || (!syllables.empty() && entry.syllables == syllables))) {
            return true;
        }
    }
    return false;
}

bool UserLexicon::contains_candidate_identity(const std::string& text, const std::string& code,
                                              const std::string& syllables) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = text_index_.find(text);
    if (found == text_index_.end()) {
        return false;
    }
    return std::any_of(found->second.begin(), found->second.end(), [&](EntryId id) {
        const Entry& entry = entries_[id];
        return !entry.deleted && entry.code == code && entry.syllables == syllables;
    });
}

std::size_t UserLexicon::entry_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entry_index_.size();
}

std::uint64_t UserLexicon::version() const { return version_.load(std::memory_order_acquire); }

} // namespace cxxime
