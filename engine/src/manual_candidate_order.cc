// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/manual_candidate_order.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <cxxime/input_limits.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>
#include <cxxime/user_data_merge.h>

#include "user_data_file.h"

namespace cxxime {
namespace {

constexpr const char* kHeader = "# zhiyi-candidate-order format=1";
constexpr std::size_t kMaxEntries = 100000;
constexpr std::size_t kMaxFileSize = 16 * 1024 * 1024;

std::size_t max_code_length(UserDictKind kind) {
    return kind == UserDictKind::WUBI ? kMaxWubiCodeLength : kMaxInputCodeLength;
}

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return fields;
}

bool parse_position(const std::string& value, std::size_t* position) {
    if (!position || value.empty() ||
        !std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > MANUAL_CANDIDATE_ORDER_MAX_ENTRIES) {
        return false;
    }
    *position = static_cast<std::size_t>(parsed - 1);
    return true;
}

std::string entry_key(const ManualCandidateOrderEntry& entry) {
    std::string key;
    key.reserve(entry.text.size() + entry.code.size() + entry.syllables.size() + 2);
    key.append(entry.code);
    key.push_back('\x1f');
    key.append(entry.text);
    key.push_back('\x1f');
    key.append(entry.syllables);
    return key;
}

std::uint64_t version_token(std::string_view contents) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char character : contents) {
        hash ^= character;
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0 ? 1 : hash;
}

} // namespace

bool ManualCandidateOrder::parse_contents(const std::string& contents, UserDictKind kind,
                                          Orders* orders) {
    if (!orders) {
        return false;
    }
    Orders loaded;
    if (!contents.empty()) {
        std::istringstream input(contents);
        std::string line;
        if (!std::getline(input, line)) {
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line != kHeader) {
            return false;
        }

        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            const std::vector<std::string> fields = split_fields(line);
            std::size_t position = 0;
            if (fields.size() != 5 || !is_valid_user_dict_code(fields[0]) ||
                !is_valid_user_dict_text(fields[1]) ||
                !is_valid_user_dict_code(fields[2]) ||
                !is_valid_user_dict_syllables(fields[3]) ||
                !parse_position(fields[4], &position)) {
                return false;
            }
            auto& entries = loaded[fields[0]];
            if (position != entries.size()) {
                return false;
            }
            entries.push_back({fields[1], fields[2], fields[3]});
        }
    }
    if (!validate_orders(loaded, kind)) {
        return false;
    }
    // Validate original positions first; removing a historical symbol must not
    // invalidate the following entries or discard unrelated ordering groups.
    for (auto group = loaded.begin(); group != loaded.end();) {
        auto& entries = group->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const auto& entry) {
                                         return !is_ordinary_candidate_text(entry.text);
                                     }),
                      entries.end());
        if (entries.empty()) {
            group = loaded.erase(group);
        } else {
            ++group;
        }
    }
    *orders = std::move(loaded);
    return true;
}

bool ManualCandidateOrder::validate_contents(const std::string& contents, UserDictKind kind) {
    Orders orders;
    return contents.size() <= kMaxFileSize && parse_contents(contents, kind, &orders);
}

bool ManualCandidateOrder::load(const std::string& path) {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    std::string contents;
    if (!read_user_data_file(path, kMaxFileSize, &contents)) {
        return false;
    }

    Orders loaded;
    if (!parse_contents(contents, kind_, &loaded)) {
        return false;
    }

    const std::uint64_t version = version_token(serialize(loaded));
    std::unique_lock<std::shared_mutex> lock(mutex_);
    orders_ = std::move(loaded);
    path_ = path;
    version_.store(version, std::memory_order_release);
    return true;
}

bool ManualCandidateOrder::merge_contents_and_save(const std::string& imported,
                                                   UserDataMergeResult* result) {
    if (!result) {
        return false;
    }
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    Orders current;
    std::string path;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        current = orders_;
        path = path_;
    }
    UserDataMergeResult merged;
    Orders next;
    if (path.empty() ||
        !merge_lexicon_resource_contents(LexiconResource::kManualCandidateOrder, kind_,
                                         serialize(current), imported, &merged) ||
        !parse_contents(merged.contents, kind_, &next) ||
        !write_user_data_file_atomically(path, merged.contents)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    orders_ = std::move(next);
    version_.store(version_token(merged.contents), std::memory_order_release);
    *result = std::move(merged);
    return true;
}

std::vector<ManualCandidateOrderEntry>
ManualCandidateOrder::entries_for(const std::string& input_code) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = orders_.find(input_code);
    return found == orders_.end() ? std::vector<ManualCandidateOrderEntry>{} : found->second;
}

bool ManualCandidateOrder::contains(const std::string& input_code, const std::string& text,
                                    const std::string& candidate_code,
                                    const std::string& syllables) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = orders_.find(input_code);
    return found != orders_.end() &&
           std::any_of(found->second.begin(), found->second.end(), [&](const auto& entry) {
               return entry.text == text && entry.code == candidate_code &&
                      entry.syllables == syllables;
           });
}

bool ManualCandidateOrder::replace_and_save(const std::string& input_code,
                                            const std::vector<ManualCandidateOrderEntry>& entries) {
    if (!is_valid_user_dict_code(input_code) || input_code.size() > max_code_length(kind_)) {
        return false;
    }
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    return replace_and_save_locked(input_code, entries);
}

bool ManualCandidateOrder::replace_and_save_if_version(
    const std::string& input_code, const std::vector<ManualCandidateOrderEntry>& entries,
    std::uint64_t expected_version, bool* version_conflict) {
    if (!version_conflict) {
        return false;
    }
    *version_conflict = false;
    if (!is_valid_user_dict_code(input_code) || input_code.size() > max_code_length(kind_)) {
        return false;
    }
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    if (version_.load(std::memory_order_acquire) != expected_version) {
        *version_conflict = true;
        return false;
    }
    return replace_and_save_locked(input_code, entries);
}

bool ManualCandidateOrder::replace_and_save_locked(
    const std::string& input_code, const std::vector<ManualCandidateOrderEntry>& entries) {
    if (std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
            return !is_ordinary_candidate_text(entry.text);
        })) {
        return false;
    }
    Orders next;
    std::string path;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        next = orders_;
        path = path_;
    }
    if (path.empty()) {
        return false;
    }
    if (entries.empty()) {
        next.erase(input_code);
    } else {
        next[input_code] = entries;
    }
    if (!validate_orders(next, kind_)) {
        return false;
    }
    const std::string contents = serialize(next);
    if (contents.size() > kMaxFileSize || !write_user_data_file_atomically(path, contents)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    orders_ = std::move(next);
    version_.store(version_token(contents), std::memory_order_release);
    return true;
}

std::uint64_t ManualCandidateOrder::version() const {
    return version_.load(std::memory_order_acquire);
}

bool ManualCandidateOrder::validate_orders(const Orders& orders, UserDictKind kind) {
    const std::size_t code_length_limit = max_code_length(kind);
    std::size_t total = 0;
    for (const auto& item : orders) {
        if (!is_valid_user_dict_code(item.first) || item.first.size() > code_length_limit ||
            item.second.empty() || item.second.size() > MANUAL_CANDIDATE_ORDER_MAX_ENTRIES) {
            return false;
        }
        std::unordered_set<std::string> seen;
        for (const auto& entry : item.second) {
            if (!is_valid_user_dict_text(entry.text) || !is_valid_user_dict_code(entry.code) ||
                entry.code.size() > code_length_limit ||
                !is_valid_user_dict_syllables(entry.syllables) ||
                !seen.insert(entry_key(entry)).second) {
                return false;
            }
        }
        total += item.second.size();
        if (total > kMaxEntries) {
            return false;
        }
    }
    return true;
}

std::string ManualCandidateOrder::serialize(const Orders& orders) {
    std::vector<std::string> codes;
    codes.reserve(orders.size());
    for (const auto& item : orders) {
        codes.push_back(item.first);
    }
    std::sort(codes.begin(), codes.end());

    std::ostringstream output;
    output << kHeader << '\n';
    for (const auto& code : codes) {
        const auto& entries = orders.at(code);
        for (std::size_t position = 0; position < entries.size(); ++position) {
            const auto& entry = entries[position];
            output << code << '\t' << entry.text << '\t' << entry.code << '\t'
                   << entry.syllables << '\t' << position + 1 << '\n';
        }
    }
    return output.str();
}

} // namespace cxxime
