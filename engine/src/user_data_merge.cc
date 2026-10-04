// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/user_data_merge.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cxxime/candidate_preference.h>
#include <cxxime/composition_learning.h>
#include <cxxime/disabled_system_lexicon.h>
#include <cxxime/input_limits.h>
#include <cxxime/manual_candidate_order.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>
#include <cxxime/user_lexicon.h>

namespace cxxime {
namespace {

constexpr char kCandidateOrderHeader[] = "# zhiyi-candidate-order format=1";
constexpr std::size_t kMaxMergedFileSize = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCandidateOrderFileSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCandidateOrderEntries = 100000;

enum class RecordFormat {
    kCandidatePreference,
    kDisabledSystemLexicon,
    kCompositionLearning,
};

std::vector<std::string> lines(const std::string& contents) {
    std::vector<std::string> result;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            result.push_back(std::move(line));
        }
    }
    return result;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = line.find('\t', start);
        result.push_back(line.substr(start, separator - start));
        if (separator == std::string::npos) {
            return result;
        }
        start = separator + 1;
    }
}

bool parse_unsigned(const std::string& value, std::uint64_t* parsed) {
    if (!parsed || value.empty() || value.front() == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long number = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *parsed = static_cast<std::uint64_t>(number);
    return true;
}

std::string make_key(const std::vector<std::string>& item, std::size_t first, std::size_t second) {
    return item[first] + '\n' + item[second];
}

std::string serialize(const std::map<std::string, std::string>& records) {
    std::string contents;
    for (const auto& record : records) {
        contents += record.second;
        contents.push_back('\n');
    }
    return contents;
}

std::string
serialize_candidate_orders(const std::map<std::string, std::vector<std::string>>& groups) {
    std::string contents = std::string(kCandidateOrderHeader) + '\n';
    for (const auto& group : groups) {
        for (const auto& line : group.second) {
            contents += line + '\n';
        }
    }
    return contents;
}

bool valid_record_line(RecordFormat format, const std::string& line) {
    const std::string contents = line + '\n';
    if (format == RecordFormat::kCandidatePreference) {
        return CandidatePreference::validate_contents(contents);
    }
    if (format == RecordFormat::kCompositionLearning) {
        return CompositionLearningService::validate_contents(contents);
    }
    if (format == RecordFormat::kDisabledSystemLexicon) {
        return DisabledSystemLexicon::validate_contents(contents);
    }
    return false;
}

bool valid_record_contents(RecordFormat format, const std::string& contents) {
    if (format == RecordFormat::kCandidatePreference) {
        return CandidatePreference::validate_contents(contents);
    }
    if (format == RecordFormat::kCompositionLearning) {
        return CompositionLearningService::validate_contents(contents);
    }
    if (format == RecordFormat::kDisabledSystemLexicon) {
        return DisabledSystemLexicon::validate_contents(contents);
    }
    return false;
}

bool valid_lexicon_contents(LexiconResource resource, UserDictKind kind,
                            const std::string& contents) {
    switch (resource) {
    case LexiconResource::kUserLexicon:
        return UserLexicon::validate_contents(contents, kind);
    case LexiconResource::kCandidatePreference:
        return CandidatePreference::validate_contents(contents);
    case LexiconResource::kDisabledSystemLexicon:
        return DisabledSystemLexicon::validate_contents(contents);
    case LexiconResource::kManualCandidateOrder:
        return ManualCandidateOrder::validate_contents(contents, kind);
    }
    return false;
}

std::string merge_learning_line(const std::string& current, const std::string& imported,
                                std::size_t count_field, std::size_t sequence_field) {
    std::vector<std::string> current_fields = fields(current);
    std::vector<std::string> imported_fields = fields(imported);
    std::uint64_t current_count = 0;
    std::uint64_t imported_count = 0;
    std::uint64_t current_sequence = 0;
    std::uint64_t imported_sequence = 0;
    if (current_fields.size() <= sequence_field || imported_fields.size() <= sequence_field ||
        !parse_unsigned(current_fields[count_field], &current_count) ||
        !parse_unsigned(imported_fields[count_field], &imported_count) ||
        !parse_unsigned(current_fields[sequence_field], &current_sequence) ||
        !parse_unsigned(imported_fields[sequence_field], &imported_sequence)) {
        return imported;
    }
    imported_fields[count_field] = std::to_string((std::max)(current_count, imported_count));
    imported_fields[sequence_field] =
        std::to_string((std::max)(current_sequence, imported_sequence));
    std::ostringstream output;
    for (std::size_t index = 0; index < imported_fields.size(); ++index) {
        if (index != 0) {
            output << '\t';
        }
        output << imported_fields[index];
    }
    return output.str();
}

bool merge_user_lexicon(UserDictKind kind, const std::string& current,
                        const std::string& imported, UserDataMergeResult* result) {
    std::vector<std::string> records = lines(current);
    std::map<std::string, std::size_t> positions;
    std::size_t serialized_size = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto item = fields(records[index]);
        if (!is_ordinary_candidate_text(item[0])) {
            records[index].clear();
            continue;
        }
        positions[make_key(item, 0, 1)] = index;
        serialized_size += records[index].size() + 1;
    }
    if (serialized_size > kMaxMergedFileSize) {
        return false;
    }
    for (const std::string& line : lines(imported)) {
        const auto item = fields(line);
        if (!UserLexicon::validate_contents(line + '\n', kind) ||
            !is_ordinary_candidate_text(item[0])) {
            ++result->skipped_count;
            continue;
        }
        const std::string key = make_key(item, 0, 1);
        const auto existing = positions.find(key);
        const std::size_t replaced_size =
            existing == positions.end() ? 0 : records[existing->second].size() + 1;
        if (serialized_size - replaced_size + line.size() + 1 > kMaxMergedFileSize) {
            ++result->skipped_count;
            continue;
        }
        if (existing != positions.end()) {
            records[existing->second].clear();
            serialized_size -= replaced_size;
        }
        positions[key] = records.size();
        records.push_back(line);
        serialized_size += line.size() + 1;
        ++result->imported_count;
    }
    for (const auto& line : records) {
        if (!line.empty()) {
            result->contents += line + '\n';
        }
    }
    return UserLexicon::validate_contents(result->contents, kind);
}

bool learning_record_less_valuable(const std::pair<const std::string, std::string>& left,
                                   const std::pair<const std::string, std::string>& right) {
    const auto left_fields = fields(left.second);
    const auto right_fields = fields(right.second);
    std::uint64_t left_count = 0;
    std::uint64_t right_count = 0;
    std::uint64_t left_sequence = 0;
    std::uint64_t right_sequence = 0;
    parse_unsigned(left_fields[3], &left_count);
    parse_unsigned(right_fields[3], &right_count);
    parse_unsigned(left_fields[4], &left_sequence);
    parse_unsigned(right_fields[4], &right_sequence);
    if (left_count != right_count) {
        return left_count < right_count;
    }
    if (left_sequence != right_sequence) {
        return left_sequence < right_sequence;
    }
    return left.first < right.first;
}

bool merge_records(RecordFormat format, const std::string& current,
                   const std::string& imported, UserDataMergeResult* result) {
    const bool disabled = format == RecordFormat::kDisabledSystemLexicon;
    const bool preference = format == RecordFormat::kCandidatePreference;
    const bool composition = format == RecordFormat::kCompositionLearning;
    const auto key_for = [&](const std::string& line, const std::vector<std::string>& item) {
        return disabled ? line : make_key(item, 1, 0);
    };
    std::map<std::string, std::string> records;
    std::size_t serialized_size = 0;
    for (const std::string& line : lines(current)) {
        const auto item = fields(line);
        if (valid_record_line(format, line) &&
            (disabled || is_ordinary_candidate_text(item[0]))) {
            records[key_for(line, item)] = line;
        }
    }
    for (const auto& record : records) {
        serialized_size += record.second.size() + 1;
    }
    const std::size_t max_file_size =
        composition ? CompositionLearningService::kMaxFileSize : kMaxMergedFileSize;
    if (serialized_size > max_file_size) {
        return false;
    }
    std::map<std::string, std::size_t> imported_occurrences;
    for (const std::string& line : lines(imported)) {
        const auto item = fields(line);
        if (!valid_record_line(format, line) ||
            (!disabled && !is_ordinary_candidate_text(item[0]))) {
            ++result->skipped_count;
            continue;
        }
        const std::string key = key_for(line, item);
        const auto existing = records.find(key);
        const std::size_t existing_size =
            existing == records.end() ? 0 : existing->second.size() + 1;
        const std::string merged_line = existing != records.end() && (preference || composition)
                                            ? merge_learning_line(existing->second, line, 3, 4)
                                            : line;
        if (!composition &&
            serialized_size - existing_size + merged_line.size() + 1 > max_file_size) {
            ++result->skipped_count;
            continue;
        }
        if (existing != records.end() && (preference || composition)) {
            records[key] = merged_line;
        } else {
            records[key] = line;
        }
        serialized_size = serialized_size - existing_size + records[key].size() + 1;
        ++imported_occurrences[key];
        ++result->imported_count;
    }
    if (composition && (records.size() > CompositionLearningService::kMaxRecordCount ||
                        serialized_size > max_file_size)) {
        std::vector<std::string> eviction_order;
        eviction_order.reserve(records.size());
        for (const auto& record : records) {
            eviction_order.push_back(record.first);
        }
        std::sort(
            eviction_order.begin(), eviction_order.end(), [&](const auto& left, const auto& right) {
                return learning_record_less_valuable(*records.find(left), *records.find(right));
            });
        for (const auto& key : eviction_order) {
            if (records.size() <= CompositionLearningService::kMaxRecordCount &&
                serialized_size <= max_file_size) {
                break;
            }
            const auto victim = records.find(key);
            const auto imported_count = imported_occurrences.find(key);
            if (imported_count != imported_occurrences.end()) {
                result->imported_count -= imported_count->second;
                result->skipped_count += imported_count->second;
                imported_occurrences.erase(imported_count);
            }
            serialized_size -= victim->second.size() + 1;
            records.erase(victim);
        }
    }
    result->contents = serialize(records);
    return valid_record_contents(format, result->contents);
}

bool merge_candidate_order(const std::string& current, const std::string& imported,
                           UserDictKind kind, UserDataMergeResult* result) {
    const std::size_t max_code_length =
        kind == UserDictKind::WUBI ? kMaxWubiCodeLength : kMaxInputCodeLength;
    std::map<std::string, std::vector<std::string>> groups;
    std::size_t total_entries = 0;
    std::size_t serialized_size = std::char_traits<char>::length(kCandidateOrderHeader) + 1;
    auto collect = [&](const std::string& contents, bool replace) {
        std::map<std::string, std::vector<std::string>> incoming_groups;
        const auto source_lines = lines(contents);
        if (source_lines.empty()) {
            return;
        }
        if (source_lines.front() != kCandidateOrderHeader) {
            if (replace) {
                result->skipped_count += source_lines.size();
            }
            return;
        }
        std::map<std::string, std::unordered_set<std::string>> seen;
        for (std::size_t index = 1; index < source_lines.size(); ++index) {
            const auto item = fields(source_lines[index]);
            std::uint64_t position = 0;
            const bool valid =
                item.size() == 5 && is_valid_user_dict_code(item[0]) &&
                item[0].size() <= max_code_length && is_ordinary_candidate_text(item[1]) &&
                is_valid_user_dict_code(item[2]) && item[2].size() <= max_code_length &&
                is_valid_user_dict_syllables(item[3]) && parse_unsigned(item[4], &position) &&
                position != 0 && position <= MANUAL_CANDIDATE_ORDER_MAX_ENTRIES;
            const std::string entry_key =
                valid ? item[1] + '\n' + item[2] + '\n' + item[3] : std::string();
            auto& group = valid ? incoming_groups[item[0]] : incoming_groups[std::string()];
            if (valid && group.size() < MANUAL_CANDIDATE_ORDER_MAX_ENTRIES &&
                seen[item[0]].insert(entry_key).second) {
                group.push_back(item[0] + '\t' + item[1] + '\t' + item[2] + '\t' + item[3] + '\t' +
                                std::to_string(group.size() + 1));
            } else if (replace) {
                ++result->skipped_count;
            }
        }
        incoming_groups.erase(std::string());
        for (auto& group : incoming_groups) {
            std::string candidate = std::string(kCandidateOrderHeader) + '\n';
            for (const auto& line : group.second) {
                candidate += line + '\n';
            }
            if (ManualCandidateOrder::validate_contents(candidate, kind)) {
                if (!replace) {
                    total_entries += group.second.size();
                    for (const auto& line : group.second) {
                        serialized_size += line.size() + 1;
                    }
                    groups[group.first] = std::move(group.second);
                    continue;
                }
                const auto existing = groups.find(group.first);
                const bool had_existing = existing != groups.end();
                std::vector<std::string> previous =
                    had_existing ? existing->second : std::vector<std::string>();
                std::size_t previous_size = 0;
                for (const auto& line : previous) {
                    previous_size += line.size() + 1;
                }
                std::size_t imported_size = 0;
                for (const auto& line : group.second) {
                    imported_size += line.size() + 1;
                }
                const std::size_t next_entries =
                    total_entries - previous.size() + group.second.size();
                const std::size_t next_size = serialized_size - previous_size + imported_size;
                if (next_entries <= kMaxCandidateOrderEntries &&
                    next_size <= kMaxCandidateOrderFileSize) {
                    groups[group.first] = group.second;
                    total_entries = next_entries;
                    serialized_size = next_size;
                    result->imported_count += group.second.size();
                } else {
                    result->skipped_count += group.second.size();
                }
            } else if (replace) {
                result->skipped_count += group.second.size();
            }
        }
    };
    collect(current, false);
    collect(imported, true);
    result->contents = serialize_candidate_orders(groups);
    return ManualCandidateOrder::validate_contents(result->contents, kind);
}

} // namespace

bool merge_lexicon_resource_contents(LexiconResource resource, UserDictKind kind,
                                     const std::string& current, const std::string& imported,
                                     UserDataMergeResult* result) {
    if (!result || current.size() > kMaxMergedFileSize ||
        !valid_lexicon_contents(resource, kind, current)) {
        return false;
    }
    UserDataMergeResult merged;
    bool succeeded = false;
    switch (resource) {
    case LexiconResource::kUserLexicon:
        succeeded = merge_user_lexicon(kind, current, imported, &merged);
        break;
    case LexiconResource::kCandidatePreference:
        succeeded = merge_records(RecordFormat::kCandidatePreference, current, imported, &merged);
        break;
    case LexiconResource::kDisabledSystemLexicon:
        succeeded =
            merge_records(RecordFormat::kDisabledSystemLexicon, current, imported, &merged);
        break;
    case LexiconResource::kManualCandidateOrder:
        succeeded = merge_candidate_order(current, imported, kind, &merged);
        break;
    }
    if (!succeeded || merged.contents.size() > kMaxMergedFileSize) {
        return false;
    }
    *result = std::move(merged);
    return true;
}

bool merge_composition_learning_contents(const std::string& current,
                                         const std::string& imported,
                                         UserDataMergeResult* result) {
    if (!result || current.size() > kMaxMergedFileSize ||
        !valid_record_contents(RecordFormat::kCompositionLearning, current)) {
        return false;
    }
    UserDataMergeResult merged;
    if (!merge_records(RecordFormat::kCompositionLearning, current, imported, &merged) ||
        merged.contents.size() > kMaxMergedFileSize) {
        return false;
    }
    *result = std::move(merged);
    return true;
}

} // namespace cxxime
