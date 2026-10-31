// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/punctuation.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <utility>

#include <json.hpp>

#include <cxxime/logging.h>
#include <cxxime/output_composer.h>

namespace cxxime {

namespace {

constexpr int kMaxPunctuationCharacters = 8;

// The code points of `text`, or nullopt when it is not valid UTF-8.
std::optional<std::vector<char32_t>> code_points(const std::string& text) {
    std::vector<char32_t> points;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                                     : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) return std::nullopt;
        char32_t point = length == 1 ? lead : lead & (0x7F >> length);
        for (int k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return std::nullopt;
            point = (point << 6) | (next & 0x3F);
        }
        points.push_back(point);
        i += length;
    }
    return points;
}

std::optional<PunctEntry> entry_from_json(const nlohmann::json& value) {
    PunctEntry entry{};
    if (value.contains("commit") && value["commit"].is_string()) {
        entry.type = PunctType::COMMIT;
        entry.commit = value["commit"].get<std::string>();
    } else if (value.contains("pair") && value["pair"].is_array()) {
        entry.type = PunctType::PAIR;
        for (const auto& item : value["pair"]) {
            if (item.is_string()) entry.pair.push_back(item.get<std::string>());
        }
        if (entry.pair.size() != 2) return std::nullopt;
    } else if (value.contains("alternatives") && value["alternatives"].is_array()) {
        entry.type = PunctType::ALTERNATIVES;
        for (const auto& item : value["alternatives"]) {
            if (item.is_string()) entry.alternatives.push_back(item.get<std::string>());
        }
        if (entry.alternatives.empty()) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return entry;
}

void parse_section(const nlohmann::json& j, const char* section,
                   std::unordered_map<std::string, PunctEntry>& target) {
    if (!j.contains(section) || !j[section].is_object()) return;
    for (auto it = j[section].begin(); it != j[section].end(); ++it) {
        if (!it.value().is_object()) continue;
        if (auto entry = entry_from_json(it.value())) {
            target[it.key()] = std::move(*entry);
        } else {
            CXXIME_LOG(L"Punct: unknown entry type for key '%S'", it.key().c_str());
        }
    }
}

}  // namespace

bool is_custom_punctuation_key(char key) {
    return key != '\0' && kCustomPunctuationKeys.find(key) != std::string_view::npos;
}

bool valid_punctuation_text(const std::string& text) {
    const auto points = code_points(text);
    if (!points || points->empty() ||
        points->size() > static_cast<std::size_t>(kMaxPunctuationCharacters)) {
        return false;
    }
    return std::none_of(points->begin(), points->end(), [](char32_t point) {
        return point < 0x20 || point == 0x7F || (point >= 0x80 && point < 0xA0) ||
               (point >= 0xD800 && point <= 0xDFFF) || point > 0x10FFFF;
    });
}

bool valid_punctuation_entry(const PunctEntry& entry) {
    switch (entry.type) {
    case PunctType::COMMIT:
        return valid_punctuation_text(entry.commit);
    case PunctType::PAIR:
        return entry.pair.size() == 2 && valid_punctuation_text(entry.pair[0]) &&
               valid_punctuation_text(entry.pair[1]);
    case PunctType::ALTERNATIVES:
        return false;
    }
    return false;
}

bool is_ascii_text(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
}

std::optional<PunctMapping> parse_punctuation_json(const std::string& json_text) {
    try {
        const nlohmann::json j = nlohmann::json::parse(json_text);
        if (!j.is_object()) return std::nullopt;
        PunctMapping mapping;
        parse_section(j, "half_shape", mapping.half_shape);
        parse_section(j, "full_shape", mapping.full_shape);
        if (j.contains("join_code") && j["join_code"].is_string()) {
            mapping.join_code = j["join_code"].get<std::string>();
        }
        for (char ch = 0x21; ch <= 0x7E; ++ch) {
            if (std::isalnum(static_cast<unsigned char>(ch))) continue;
            mapping.full_shape.try_emplace(
                std::string(1, ch),
                PunctEntry{PunctType::COMMIT, OutputComposer::to_full_width(ch), {}, {}});
        }
        return mapping;
    } catch (const nlohmann::json::exception& e) {
        CXXIME_LOG(L"Punct: parse error: %S", e.what());
        return std::nullopt;
    }
}

std::shared_ptr<PunctMapping> load_punctuation_file(const std::string& path) {
    if (path.empty()) return nullptr;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        CXXIME_LOG(L"Punct: file not found: %S", path.c_str());
        return nullptr;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto mapping = parse_punctuation_json(text);
    if (!mapping) return nullptr;
    return std::make_shared<PunctMapping>(std::move(*mapping));
}

PunctMapping apply_punctuation_config(PunctMapping table,
                                      const std::vector<PunctuationKeyConfig>& keys) {
    for (const PunctuationKeyConfig& custom : keys) {
        if (!is_custom_punctuation_key(custom.key)) continue;
        const std::string key(1, custom.key);
        if (custom.half_shape && valid_punctuation_entry(*custom.half_shape)) {
            table.half_shape[key] = *custom.half_shape;
        }
        if (custom.full_shape && valid_punctuation_entry(*custom.full_shape)) {
            table.full_shape[key] = *custom.full_shape;
        }
        if (custom.join_code) {
            const auto at = table.join_code.find(custom.key);
            if (*custom.join_code && at == std::string::npos) {
                table.join_code += custom.key;
            } else if (!*custom.join_code && at != std::string::npos) {
                table.join_code.erase(at, 1);
            }
        }
    }
    return table;
}

std::string punctuation_entry_text(const PunctEntry& entry) {
    switch (entry.type) {
    case PunctType::COMMIT:
        return entry.commit;
    case PunctType::PAIR:
        return entry.pair.empty() ? std::string{} : entry.pair[0];
    case PunctType::ALTERNATIVES:
        return entry.alternatives.empty() ? std::string{} : entry.alternatives[0];
    }
    return {};
}


namespace {

enum class Side { kNone, kLeft, kRight };

// The Chinese marks the voice model writes and the keys that type them by default; a pair key's
// left or right half, and whether an English value gets a space after it.
struct VoiceMark {
    const char* text;
    char key;
    Side side;
    bool spaced;
};

constexpr VoiceMark kVoiceMarks[] = {
    {"\xEF\xBC\x8C", ',', Side::kNone, true},  // ，
    {"\xE3\x80\x82", '.', Side::kNone, true},  // 。
    {"\xEF\xBC\x9F", '?', Side::kNone, true},  // ？
    {"\xEF\xBC\x81", '!', Side::kNone, true},  // ！
    {"\xEF\xBC\x9A", ':', Side::kNone, true},  // ：
    {"\xEF\xBC\x9B", ';', Side::kNone, true},  // ；
    {"\xE3\x80\x81", '\\', Side::kNone, true},  // 、
    {"\xE2\x80\x9C", '\"', Side::kLeft, false},  // “
    {"\xE2\x80\x9D", '\"', Side::kRight, false},  // ”
    {"\xE2\x80\x98", '\'', Side::kLeft, false},  // ‘
    {"\xE2\x80\x99", '\'', Side::kRight, false},  // ’
    {"\xEF\xBC\x88", '(', Side::kLeft, false},  // （
    {"\xEF\xBC\x89", ')', Side::kRight, false},  // ）
    {"\xE3\x80\x8A", '<', Side::kLeft, false},  // 《
    {"\xE3\x80\x8B", '>', Side::kRight, false},  // 》
    {"\xE3\x80\x90", '[', Side::kLeft, false},  // 【
    {"\xE3\x80\x91", ']', Side::kRight, false},  // 】
    {"\xE2\x80\xA6\xE2\x80\xA6", '^', Side::kNone, false},  // ……
    {"\xE2\x80\x94\xE2\x80\x94", '_', Side::kNone, false},  // ——
};

}  // namespace

std::string voice_punctuation(const std::string& text, const PunctMapping& table, bool full_shape) {
    const auto& column = full_shape ? table.full_shape : table.half_shape;
    std::string out;
    out.reserve(text.size());
    bool spaced = false;
    for (std::size_t i = 0; i < text.size();) {
        const VoiceMark* mark = nullptr;
        for (const VoiceMark& candidate : kVoiceMarks) {
            if (text.compare(i, std::char_traits<char>::length(candidate.text), candidate.text) == 0) {
                mark = &candidate;
                break;
            }
        }
        const auto entry = mark ? column.find(std::string(1, mark->key)) : column.end();
        if (entry == column.end()) {
            const std::size_t length = mark ? std::char_traits<char>::length(mark->text) : 1;
            out.append(text, i, length);
            i += length;
            continue;
        }
        const PunctEntry& value = entry->second;
        std::string typed = value.type == PunctType::PAIR && value.pair.size() == 2
                                ? value.pair[mark->side == Side::kRight ? 1 : 0]
                                : punctuation_entry_text(value);
        if (mark->spaced && !typed.empty() && is_ascii_text(typed)) {
            typed += ' ';
            spaced = true;
        }
        out += typed;
        i += std::char_traits<char>::length(mark->text);
    }
    if (spaced) {
        for (std::size_t at = out.find("  "); at != std::string::npos; at = out.find("  ", at)) {
            out.erase(at, 1);
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

}  // namespace cxxime
