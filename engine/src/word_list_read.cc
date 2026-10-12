// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Reading user word lists (word_list.h): the supported formats, as written by the programs
// that make them.

#include <cxxime/word_list.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string_view>

#include <windows.h>

#include <cxxime/segmentor.h>

namespace cxxime {

namespace {

// ─── Text encodings ──────────────────────────────────────────────────────────

std::string wide_to_utf8(const wchar_t* text, std::size_t length) {
    if (length == 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr,
                            nullptr);
    }
    return out;
}

std::string utf16le_to_utf8(const char* data, std::size_t bytes) {
    std::wstring wide(bytes / 2, L'\0');
    for (std::size_t i = 0; i < wide.size(); ++i) {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(data[2 * i]) |
                                       (static_cast<unsigned char>(data[2 * i + 1]) << 8));
    }
    return wide_to_utf8(wide.data(), wide.size());
}

std::string utf16be_to_utf8(const char* data, std::size_t bytes) {
    std::wstring wide(bytes / 2, L'\0');
    for (std::size_t i = 0; i < wide.size(); ++i) {
        wide[i] = static_cast<wchar_t>((static_cast<unsigned char>(data[2 * i]) << 8) |
                                       static_cast<unsigned char>(data[2 * i + 1]));
    }
    return wide_to_utf8(wide.data(), wide.size());
}

bool valid_utf8(std::string_view text) {
    return text.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), nullptr, 0) > 0;
}

// Text files: UTF-8 (with or without BOM), UTF-16 with a BOM or without one (every other
// byte zero), else GBK (older Chinese Windows programs).
std::string decode_text(const std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return utf16le_to_utf8(bytes.data() + 2, bytes.size() - 2);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        return utf16be_to_utf8(bytes.data() + 2, bytes.size() - 2);
    }
    if (bytes.size() >= 4) {
        std::size_t odd_zero = 0, even_zero = 0;
        const std::size_t sample = (std::min)(bytes.size() & ~std::size_t{1}, std::size_t{4096});
        for (std::size_t i = 0; i < sample; i += 2) {
            even_zero += bytes[i] == '\0' ? 1 : 0;
            odd_zero += bytes[i + 1] == '\0' ? 1 : 0;
        }
        if (odd_zero * 4 > sample / 2 && even_zero == 0) {
            return utf16le_to_utf8(bytes.data(), bytes.size() & ~std::size_t{1});
        }
        if (even_zero * 4 > sample / 2 && odd_zero == 0) {
            return utf16be_to_utf8(bytes.data(), bytes.size() & ~std::size_t{1});
        }
    }
    if (valid_utf8(bytes)) return bytes;
    const int size = MultiByteToWideChar(936, 0, bytes.data(), static_cast<int>(bytes.size()),
                                         nullptr, 0);
    if (size <= 0) return bytes;
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(936, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), size);
    return wide_to_utf8(wide.data(), wide.size());
}

// ─── Small text helpers ──────────────────────────────────────────────────────

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return lines;
}

std::vector<std::string_view> split(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t end = s.find(separator);
        parts.push_back(s.substr(0, end));
        if (end == std::string_view::npos) break;
        s.remove_prefix(end + 1);
    }
    return parts;
}

// Whitespace-separated tokens (spaces, tabs, the ideographic space U+3000).
std::vector<std::string_view> tokens(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ' ' || s[i] == '\t') {
            ++i;
            continue;
        }
        if (s.compare(i, 3, "\xE3\x80\x80") == 0) {
            i += 3;
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s.compare(j, 3, "\xE3\x80\x80") != 0) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool is_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

bool has_letter(std::string_view s) {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
}

bool parse_number(std::string_view s, double* value) {
    if (s.empty() || s.size() > 24) return false;
    std::string text(s);
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return false;
    *value = v;
    return true;
}

std::size_t codepoint_count(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

const PinyinSegmentor& segmentor() {
    static const PinyinSegmentor instance;
    return instance;
}

// "a'li'ba'ba", "a li ba ba", "a'li ba-ba", "lü" -> "a:li:ba:ba" / "lv"; empty when a part is
// not a syllable. A run without separators ("alibaba") is split into syllables when exactly
// `expected` of them spell it (the longest syllable first, as most programs write them).
std::string pinyin_syllables(std::string_view code, std::size_t expected) {
    std::string normalized;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(code[i]);
        if (code.compare(i, 2, "\xC3\xBC") == 0) {  // ü
            normalized += 'v';
            ++i;
        } else if (c == '\'' || c == ' ' || c == '-' || c == '\t' || c == ':') {
            normalized += ' ';
        } else if (c < 0x80 && std::isalpha(c)) {
            normalized += static_cast<char>(std::tolower(c));
        } else {
            return {};
        }
    }
    std::vector<std::string> parts;
    for (std::string_view part : tokens(normalized)) {
        std::string syllable(part);
        if (syllable == "lue") syllable = "lve";
        if (syllable == "nue") syllable = "nve";
        parts.push_back(std::move(syllable));
    }
    if (parts.size() == 1 && expected > 1) {
        // One run: split it into `expected` syllables (longest first, then backtracking).
        const std::string run = parts.front();
        std::vector<std::string> split_run;
        std::function<bool(std::size_t)> walk = [&](std::size_t position) -> bool {
            if (position == run.size()) return split_run.size() == expected;
            if (split_run.size() == expected) return false;
            for (std::size_t length = (std::min<std::size_t>)(6, run.size() - position); length >= 1; --length) {
                const std::string syllable = run.substr(position, length);
                if (!segmentor().is_syllable(syllable) && syllable != "yo") continue;
                split_run.push_back(syllable);
                if (walk(position + length)) return true;
                split_run.pop_back();
            }
            return false;
        };
        if (!walk(0)) return {};
        parts = std::move(split_run);
    }
    std::string out;
    for (const std::string& syllable : parts) {
        if (syllable.empty() || syllable.size() > 6) return {};
        if (!out.empty()) out += ':';
        out += syllable;
    }
    return out;
}

bool is_wubi_code(std::string_view code) {
    return !code.empty() && code.size() <= 4 &&
           std::all_of(code.begin(), code.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

std::string lowercase_extension(const std::string& file_name) {
    std::string name = file_name;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ─── Sogou cell dictionary (.scel) ───────────────────────────────────────────
//
// Little-endian, text in UTF-16LE. At 0x120 the number of pinyin groups and of words, at 0x130
// the name; the pinyin table at 0x1540 (count, then index, byte length, syllable); the words from
// 0x2628 (0x26C4 when byte 4 is 'E'): per group the number of words, the byte length of the
// pinyin indexes and the indexes, then per word its byte length, the word, and extra data
// (its first two bytes are a serial number, not a frequency).

constexpr char kScelMagic[] = "\x40\x15\x00\x00";

bool is_scel(const std::string& bytes) {
    return bytes.size() > 0x2628 && std::memcmp(bytes.data(), kScelMagic, 4) == 0 &&
           (bytes[4] == 'D' || bytes[4] == 'E') && bytes[5] == 'C' && bytes[6] == 'S';
}

std::uint16_t u16(const std::string& b, std::size_t p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[p]) |
                                      (static_cast<unsigned char>(b[p + 1]) << 8));
}

std::uint32_t u32(const std::string& b, std::size_t p) {
    return static_cast<std::uint32_t>(u16(b, p)) | (static_cast<std::uint32_t>(u16(b, p + 2)) << 16);
}

std::string utf16_field(const std::string& b, std::size_t offset, std::size_t bytes) {
    if (offset + bytes > b.size()) return {};
    std::size_t length = 0;
    while (length + 1 < bytes && (b[offset + length] != '\0' || b[offset + length + 1] != '\0')) length += 2;
    return utf16le_to_utf8(b.data() + offset, length);
}

WordListReadError parse_scel(const std::string& b, WordListSource* source) {
    source->format = WordListFormat::kSogouCell;
    source->language = WordListLanguage::kChinese;
    source->name = std::string(trim(utf16_field(b, 0x130, 0x338 - 0x130)));
    const std::size_t words_offset = b[4] == 'E' ? 0x26C4 : 0x2628;
    const std::uint32_t declared_groups = u32(b, 0x120);
    const std::uint32_t declared_words = u32(b, 0x124);

    std::map<std::uint16_t, std::string> syllables;
    std::size_t p = 0x1544;
    while (p + 4 <= words_offset) {
        const std::uint16_t index = u16(b, p);
        const std::uint16_t length = u16(b, p + 2);
        p += 4;
        if (length == 0 || p + length > words_offset) break;
        std::string syllable = utf16le_to_utf8(b.data() + p, length);
        p += length;
        for (char& c : syllable) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (syllable == "lue") syllable = "lve";
        if (syllable == "nue") syllable = "nve";
        syllables[index] = std::move(syllable);
    }
    if (syllables.empty()) return WordListReadError::kFormat;

    p = words_offset;
    std::uint32_t groups = 0;
    while (p + 4 <= b.size() && (declared_groups == 0 || groups < declared_groups)) {
        const std::uint16_t same = u16(b, p);
        const std::uint16_t pinyin_bytes = u16(b, p + 2);
        p += 4;
        if (same == 0 || pinyin_bytes == 0 || pinyin_bytes % 2 != 0 || p + pinyin_bytes > b.size()) break;
        std::string reading;
        bool known = true;
        for (std::size_t i = 0; i < pinyin_bytes; i += 2) {
            const auto it = syllables.find(u16(b, p + i));
            if (it == syllables.end()) {
                known = false;
                continue;
            }
            if (!reading.empty()) reading += ':';
            reading += it->second;
        }
        p += pinyin_bytes;
        bool broken = false;
        for (std::uint16_t w = 0; w < same; ++w) {
            if (p + 2 > b.size()) {
                broken = true;
                break;
            }
            const std::uint16_t text_bytes = u16(b, p);
            p += 2;
            if (p + text_bytes + 2 > b.size()) {
                broken = true;
                break;
            }
            std::string text = utf16le_to_utf8(b.data() + p, text_bytes);
            p += text_bytes;
            const std::uint16_t extra = u16(b, p);
            p += 2 + extra;
            if (p > b.size()) {
                broken = true;
                break;
            }
            if (!known || codepoint_count(text) != pinyin_bytes / 2) {
                ++source->skipped_lines;
                continue;
            }
            source->words.push_back({std::move(text), reading, {}, -1.0});
        }
        if (broken) break;
        ++groups;
        if (declared_words != 0 && source->words.size() + source->skipped_lines >= declared_words) break;
    }
    return source->words.empty() ? WordListReadError::kNoWords : WordListReadError::kNone;
}

// ─── Rime dictionary (.dict.yaml) ────────────────────────────────────────────

WordListReadError parse_rime(std::string_view text, WordListSource* source) {
    source->format = WordListFormat::kRime;
    std::vector<std::string_view> lines = split_lines(text);
    std::vector<std::string> columns = {"text", "code", "weight"};
    std::size_t body = 0;
    // A YAML header ("---" ... "...") gives the name and the column order.
    bool header = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = trim(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        if (line == "---") header = true;
        break;
    }
    if (header) {
        bool in_columns = false;
        std::vector<std::string> declared;
        std::size_t i = 0;
        for (; i < lines.size(); ++i) {
            const std::string_view raw = lines[i];
            const std::string_view line = trim(raw);
            if (line == "...") break;
            if (line.empty() || line[0] == '#') continue;
            if (in_columns) {
                if (line.size() > 2 && line[0] == '-' && line[1] == ' ' && raw[0] == ' ') {
                    declared.emplace_back(trim(line.substr(2)));
                    continue;
                }
                in_columns = false;
            }
            if (line.rfind("name:", 0) == 0) {
                std::string_view name = trim(line.substr(5));
                if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'')) {
                    name = name.substr(1, name.size() - 2);
                }
                source->name = std::string(name);
            } else if (line == "columns:") {
                in_columns = true;
            }
        }
        if (!declared.empty()) columns = declared;
        body = i < lines.size() ? i + 1 : lines.size();
    }
    const auto column = [&](const char* name) {
        const auto it = std::find(columns.begin(), columns.end(), name);
        return it == columns.end() ? std::string::npos : static_cast<std::size_t>(it - columns.begin());
    };
    const std::size_t text_column = column("text");
    const std::size_t code_column = column("code");
    const std::size_t weight_column = column("weight");
    if (text_column == std::string::npos) return WordListReadError::kFormat;

    struct Row {
        std::string_view text, code;
        double weight = -1.0;
    };
    std::vector<Row> rows;
    std::size_t english = 0, chinese = 0, pinyin_votes = 0, wubi_votes = 0;
    for (std::size_t i = body; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (trim(line).empty() || line[0] == '#') continue;
        const std::vector<std::string_view> fields = split(line, '\t');
        Row row;
        row.text = text_column < fields.size() ? trim(fields[text_column]) : std::string_view{};
        if (row.text.empty()) {
            ++source->skipped_lines;
            continue;
        }
        if (code_column < fields.size()) row.code = trim(fields[code_column]);
        if (weight_column < fields.size()) {
            double w = 0;
            std::string_view field = trim(fields[weight_column]);
            if (!field.empty() && field.back() == '%') field.remove_suffix(1);
            if (parse_number(field, &w)) row.weight = w;
        }
        if (is_ascii(row.text)) {
            english += has_letter(row.text) ? 1 : 0;
        } else {
            ++chinese;
            if (!row.code.empty()) {
                if (row.code.find(' ') != std::string_view::npos ||
                    (codepoint_count(row.text) == 1 && segmentor().is_syllable(std::string(row.code)))) {
                    ++pinyin_votes;
                } else if (is_wubi_code(row.code)) {
                    ++wubi_votes;
                }
            }
        }
        rows.push_back(row);
    }
    if (rows.empty()) return WordListReadError::kNoWords;

    if (english > chinese) {
        source->language = WordListLanguage::kEnglish;
        for (const Row& row : rows) {
            if (!is_ascii(row.text) || !has_letter(row.text)) {
                ++source->skipped_lines;
                continue;
            }
            source->words.push_back({std::string(row.text), {}, {}, row.weight});
        }
    } else {
        source->language = WordListLanguage::kChinese;
        const bool wubi = wubi_votes > pinyin_votes;
        for (const Row& row : rows) {
            if (is_ascii(row.text)) {
                ++source->skipped_lines;
                continue;
            }
            WordListWord word{std::string(row.text), {}, {}, row.weight};
            if (wubi) {
                if (!is_wubi_code(row.code)) {
                    ++source->skipped_lines;
                    continue;
                }
                word.wubi = std::string(row.code);
            } else if (!row.code.empty()) {
                word.syllables = pinyin_syllables(row.code, codepoint_count(row.text));
                if (word.syllables.empty()) {
                    ++source->skipped_lines;
                    continue;
                }
            }
            // A pinyin dictionary may leave a word's code out (Rime works it out): the reading
            // is then worked out from the characters (resolve_word_list).
            source->words.push_back(std::move(word));
        }
    }
    return source->words.empty() ? WordListReadError::kNoWords : WordListReadError::kNone;
}

// ─── Text lists ──────────────────────────────────────────────────────────────
//
// Chinese: the word and its pinyin in any order, separated by spaces or tabs, the pinyin with
// apostrophes or spaces between the syllables, an optional number (weight):
//   'a'li'ba'ba 阿里巴巴        (Sogou)
//   a'li'ba'ba 阿里巴巴 1       (QQ)
//   阿里巴巴	a li ba ba	100
// English: a word or phrase per line, optionally a weight after a tab or comma.

// An English line's word: the line, or its first field when the rest is not English (a
// word book's "abandon,放弃"). Empty when the line is not an English word.
std::string_view english_field(std::string_view line) {
    if (is_ascii(line)) return has_letter(line) ? line : std::string_view{};
    const std::size_t cut = line.find_first_of(",\t");
    if (cut == std::string_view::npos) return {};
    const std::string_view first = trim(line.substr(0, cut));
    if (!is_ascii(first) || !has_letter(first)) return {};
    // "zhi'yi<TAB>知意" is the pinyin of a Chinese word, not an English word and its meaning.
    for (std::string_view token : tokens(line.substr(cut + 1))) {
        if (!is_ascii(token)) {
            return pinyin_syllables(first, codepoint_count(token)).empty() ? first : std::string_view{};
        }
    }
    return first;
}

WordListReadError parse_text(std::string_view text, WordListSource* source) {
    const std::vector<std::string_view> lines = split_lines(text);
    std::size_t english = 0, chinese = 0;
    for (std::string_view line : lines) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (!english_field(line).empty()) {
            ++english;
        } else if (!is_ascii(line)) {
            ++chinese;
        }
    }
    if (english == 0 && chinese == 0) return WordListReadError::kNoWords;

    if (english > chinese) {
        source->format = WordListFormat::kEnglishText;
        source->language = WordListLanguage::kEnglish;
        for (std::string_view line : lines) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const std::string_view field = english_field(line);
            if (field.empty()) {
                ++source->skipped_lines;
                continue;
            }
            WordListWord word;
            if (field.size() != line.size()) {
                word.text = std::string(field);
                source->words.push_back(std::move(word));
                continue;
            }
            // "word<TAB>weight", "word,weight", "word weight": the last field when a number.
            std::string_view body = line;
            for (const char separator : {'\t', ','}) {
                const std::size_t at = body.rfind(separator);
                if (at != std::string_view::npos) {
                    double w = 0;
                    if (parse_number(trim(body.substr(at + 1)), &w)) {
                        word.weight = w;
                        body = trim(body.substr(0, at));
                    } else if (separator == ',') {
                        body = trim(body.substr(0, body.find(',')));  // "word,translation"
                    }
                    break;
                }
            }
            if (word.weight < 0) {
                const std::size_t at = body.rfind(' ');
                double w = 0;
                if (at != std::string_view::npos && parse_number(body.substr(at + 1), &w)) {
                    word.weight = w;
                    body = trim(body.substr(0, at));
                }
            }
            if (body.empty() || !has_letter(body)) {
                ++source->skipped_lines;
                continue;
            }
            word.text = std::string(body);
            source->words.push_back(std::move(word));
        }
        return source->words.empty() ? WordListReadError::kNoWords : WordListReadError::kNone;
    }

    source->format = WordListFormat::kPinyinText;
    source->language = WordListLanguage::kChinese;
    std::size_t without_pinyin = 0;
    for (std::string_view line : lines) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::string word_text;
        std::string code;
        double weight = -1.0;
        for (std::string_view token : tokens(line)) {
            double w = 0;
            if (!is_ascii(token)) {
                if (!word_text.empty()) {
                    word_text.clear();  // two words on a line: not a list entry
                    break;
                }
                word_text = std::string(token);
            } else if (parse_number(token, &w)) {
                weight = w;
            } else {
                if (!code.empty()) code += ' ';
                code += std::string(token);
            }
        }
        if (word_text.empty()) {
            ++source->skipped_lines;
            continue;
        }
        if (code.empty()) {
            ++without_pinyin;
            continue;
        }
        while (!code.empty() && code.front() == '\'') code.erase(code.begin());
        const std::string syllables = pinyin_syllables(code, codepoint_count(word_text));
        if (syllables.empty()) {
            ++source->skipped_lines;
            continue;
        }
        source->words.push_back({std::move(word_text), syllables, {}, weight});
    }
    if (source->words.empty()) {
        return without_pinyin > 0 ? WordListReadError::kNoPinyin : WordListReadError::kNoWords;
    }
    source->skipped_lines += without_pinyin;
    return WordListReadError::kNone;
}

}  // namespace

bool word_list_extension_supported(const std::string& file_name) {
    const std::string name = lowercase_extension(file_name);
    for (const char* extension : {".scel", ".yaml", ".yml", ".txt", ".csv", ".tsv"}) {
        if (ends_with(name, extension)) return true;
    }
    return false;
}

WordListReadError parse_word_list(const std::string& bytes, const std::string& file_name,
                                  WordListSource* source) {
    *source = WordListSource{};
    if (!word_list_extension_supported(file_name)) return WordListReadError::kExtension;
    if (bytes.size() > kMaxWordListBytes) return WordListReadError::kTooLarge;
    const std::string name = lowercase_extension(file_name);
    if (is_scel(bytes)) return parse_scel(bytes, source);
    if (ends_with(name, ".scel")) return WordListReadError::kFormat;
    const std::string text = decode_text(bytes);
    if (ends_with(name, ".yaml") || ends_with(name, ".yml")) return parse_rime(text, source);
    // A Rime dictionary saved under another name still starts with its header.
    for (std::string_view line : split_lines(std::string_view(text).substr(0, 4096))) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "---") return parse_rime(text, source);
        break;
    }
    return parse_text(text, source);
}

WordListReadError read_word_list(const std::wstring& path, WordListSource* source) {
    *source = WordListSource{};
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring file_name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::string utf8_name = wide_to_utf8(file_name.data(), file_name.size());
    if (!word_list_extension_supported(utf8_name)) return WordListReadError::kExtension;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return WordListReadError::kOpen;
    const std::streamoff size = file.tellg();
    if (size < 0) return WordListReadError::kOpen;
    if (static_cast<std::uint64_t>(size) > kMaxWordListBytes) return WordListReadError::kTooLarge;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (size > 0 && !file.read(bytes.data(), size)) return WordListReadError::kOpen;
    return parse_word_list(bytes, utf8_name, source);
}

}  // namespace cxxime
