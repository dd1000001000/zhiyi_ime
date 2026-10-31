// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/custom_phrases.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>

#include <windows.h>

namespace cxxime {

namespace {

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>((std::max)(0, size)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                            nullptr, nullptr);
    }
    return out;
}

std::wstring code_page_to_wide(std::string_view bytes, UINT code_page, DWORD flags) {
    if (bytes.empty()) return {};
    const int size = MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()), out.data(), size);
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == text.size()) break;
        begin = end + 1;
    }
    return lines;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

// "code,place=rest": the code (lowercase), the place and the rest; false for any other line.
bool parse_header(const std::string& line, std::string* code, int* position, std::string* rest) {
    std::size_t i = 0;
    while (i < line.size() && ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z'))) ++i;
    if (i == 0 || i >= line.size() || line[i] != ',') return false;
    std::size_t j = i + 1;
    long number = 0;
    while (j < line.size() && line[j] >= '0' && line[j] <= '9') {
        number = (std::min)(number * 10 + (line[j] - '0'), 1000L);
        ++j;
    }
    if (j == i + 1 || j >= line.size() || line[j] != '=') return false;
    *code = lower_ascii(std::string_view(line).substr(0, i));
    *position = static_cast<int>((std::clamp)(number, 1L, static_cast<long>(kMaxCustomPhrasePosition)));
    *rest = line.substr(j + 1);
    return true;
}

void finish_phrase(CustomPhrase& phrase, std::vector<CustomPhrase>& out) {
    while (!phrase.text.empty() && (phrase.text.back() == '\n' || phrase.text.back() == ' ')) {
        phrase.text.pop_back();
    }
    if (phrase.text.empty() || phrase.text.size() > kMaxCustomPhraseBytes ||
        !is_valid_custom_phrase_code(phrase.code) || out.size() >= kMaxCustomPhrases) {
        return;
    }
    out.push_back(std::move(phrase));
}

// Chinese numerals for the time: 0 零, 3 三, 10 十, 13 十三, 22 二十二.
const char* const kDigits[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};

std::string chinese_number(int n) {
    if (n == 0) return "零";
    if (n < 10) return kDigits[n];
    std::string out = n / 10 == 1 ? "" : kDigits[n / 10];
    out += "十";
    if (n % 10) out += kDigits[n % 10];
    return out;
}

// Minutes and seconds as Sogou writes them: 02 零二, 15 十五.
std::string chinese_two_digits(int n) {
    return n < 10 ? std::string("零") + (n == 0 ? "零" : kDigits[n]) : chinese_number(n);
}

// Each digit: 2026 二〇二六, "06" 〇六.
std::string chinese_digits(const std::string& digits) {
    std::string out;
    for (char ch : digits) out += kDigits[ch - '0'];
    return out;
}

std::string two(int n) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", n % 100);
    return buf;
}

int half_hour(const std::tm& t) { return t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12; }

const char* const kWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};

}  // namespace

std::string decode_phrase_file(std::string_view bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring wide(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        return wide_to_utf8(wide);
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return std::string(bytes.substr(3));
    }
    if (bytes.empty() || !code_page_to_wide(bytes, CP_UTF8, MB_ERR_INVALID_CHARS).empty()) {
        return std::string(bytes);
    }
    return wide_to_utf8(code_page_to_wide(bytes, 936, 0));  // GBK
}

bool is_valid_custom_phrase_code(std::string_view code) {
    return !code.empty() && code.size() <= kMaxCustomPhraseCodeBytes &&
           std::all_of(code.begin(), code.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
}

std::vector<CustomPhrase> parse_custom_phrases(std::string_view bytes) {
    std::vector<CustomPhrase> out;
    std::optional<CustomPhrase> open;
    for (const std::string& line : split_lines(decode_phrase_file(bytes))) {
        if (!line.empty() && line[0] == ';') continue;  // a comment
        CustomPhrase header;
        std::string rest;
        if (parse_header(line, &header.code, &header.position, &rest)) {
            if (open) finish_phrase(*open, out);
            header.text = std::move(rest);
            open = std::move(header);
            continue;
        }
        if (!open) continue;  // before any entry
        // A line of a phrase written on several: after "code,place=" or the lines before it.
        if (!open->text.empty() || !line.empty()) {
            if (!open->text.empty()) open->text += '\n';
            open->text += line;
        }
    }
    if (open) finish_phrase(*open, out);
    return out;
}

std::vector<CustomPhrase> parse_rime_custom_phrases(std::string_view bytes) {
    // Each code's rows, codes in the order they first come.
    std::vector<std::string> codes;
    std::map<std::string, std::vector<std::pair<long, std::string>>> rows;
    for (const std::string& line : split_lines(decode_phrase_file(bytes))) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::size_t tab2 = line.find('\t', tab + 1);
        std::string code = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
        code.erase(std::remove(code.begin(), code.end(), ' '), code.end());
        code = lower_ascii(code);
        if (!is_valid_custom_phrase_code(code)) continue;
        const long weight = tab2 == std::string::npos ? 0 : std::strtol(line.c_str() + tab2 + 1, nullptr, 10);
        if (rows.find(code) == rows.end()) codes.push_back(code);
        rows[code].emplace_back(weight, line.substr(0, tab));
    }
    std::vector<CustomPhrase> out;
    for (const std::string& code : codes) {
        auto& list = rows[code];
        std::stable_sort(list.begin(), list.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        int place = 0;
        for (auto& [weight, text] : list) {
            CustomPhrase phrase;
            phrase.code = code;
            phrase.position = (std::min)(++place, kMaxCustomPhrasePosition);
            phrase.text = std::move(text);
            finish_phrase(phrase, out);
        }
    }
    return out;
}

std::string serialize_custom_phrases(const std::vector<CustomPhrase>& phrases) {
    std::string out =
        "; 知意输入法快捷短语，格式同搜狗输入法：编码,位置=短语\r\n"
        "; 位置 1-9；短语写在多行时，等号后换行；# 开头的短语填入日期时间，如 #$year年$month月$day日\r\n";
    for (const CustomPhrase& phrase : phrases) {
        out += phrase.code + "," + std::to_string(phrase.position) + "=";
        if (phrase.text.find('\n') != std::string::npos) out += "\r\n";
        for (char ch : phrase.text) {
            if (ch == '\n') out += "\r\n";
            else out += ch;
        }
        out += "\r\n";
    }
    return out;
}

std::string serialize_rime_custom_phrases(const std::vector<CustomPhrase>& phrases,
                                          std::size_t* skipped) {
    std::string out =
        "# Rime table\n"
        "# coding: utf-8\n"
        "#@/db_name\tcustom_phrase.txt\n"
        "#@/db_type\ttabledb\n"
        "#\n"
        "# 知意输入法导出的快捷短语：短语<Tab>编码<Tab>权重\n";
    std::size_t left_out = 0;
    for (const CustomPhrase& phrase : phrases) {
        if (phrase.text.find('\n') != std::string::npos || phrase.text.find('\t') != std::string::npos ||
            (!phrase.text.empty() && phrase.text[0] == '#')) {
            ++left_out;
            continue;
        }
        out += phrase.text + "\t" + phrase.code + "\t" + std::to_string(10 - phrase.position) + "\n";
    }
    if (skipped) *skipped = left_out;
    return out;
}

std::string expand_custom_phrase(const std::string& text, const std::tm& t) {
    if (text.empty() || text[0] != '#') return text;
    // Longer names first ($year_yy_cn before $year_yy before $year).
    const std::pair<const char*, std::string> variables[] = {
        {"$year_yy_cn", chinese_digits(two(t.tm_year % 100))},
        {"$year_cn", chinese_digits(std::to_string(t.tm_year + 1900))},
        {"$year_yy", two(t.tm_year % 100)},
        {"$year", std::to_string(t.tm_year + 1900)},
        {"$month_mm", two(t.tm_mon + 1)},
        {"$month_cn", chinese_number(t.tm_mon + 1)},
        {"$month", std::to_string(t.tm_mon + 1)},
        {"$day_dd", two(t.tm_mday)},
        {"$day_cn", chinese_number(t.tm_mday)},
        {"$day", std::to_string(t.tm_mday)},
        {"$weekday_cn", kWeekdays[t.tm_wday % 7]},
        {"$weekday", std::to_string(t.tm_wday)},
        {"$fullhour_cn", chinese_number(t.tm_hour)},
        {"$fullhour", two(t.tm_hour)},
        {"$halfhour_cn", chinese_number(half_hour(t))},
        {"$halfhour", two(half_hour(t))},
        {"$ampm_cn", t.tm_hour < 12 ? "上午" : "下午"},
        {"$ampm", t.tm_hour < 12 ? "AM" : "PM"},
        {"$minute_cn", chinese_two_digits(t.tm_min)},
        {"$minute", two(t.tm_min)},
        {"$second_cn", chinese_two_digits(t.tm_sec)},
        {"$second", two(t.tm_sec)},
    };
    std::string out;
    for (std::size_t i = 1; i < text.size();) {
        bool replaced = false;
        if (text[i] == '$') {
            for (const auto& [name, value] : variables) {
                const std::size_t length = std::char_traits<char>::length(name);
                if (text.compare(i, length, name) == 0) {
                    out += value;
                    i += length;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out += text[i++];
    }
    return out;
}

BuiltinTime builtin_time_for(const std::string& code) {
    // Two syllables, each typed whole or begun: "shij" is shi + j, "sj" s + j.
    auto reads_as = [&](std::string_view first, std::string_view second) {
        for (std::size_t split = 1; split < code.size(); ++split) {
            const std::string_view a = std::string_view(code).substr(0, split);
            const std::string_view b = std::string_view(code).substr(split);
            if (a.size() <= first.size() && first.substr(0, a.size()) == a && b.size() <= second.size() &&
                second.substr(0, b.size()) == b) {
                return true;
            }
        }
        return false;
    };
    if (reads_as("ri", "qi")) return BuiltinTime::kDate;
    if (reads_as("shi", "jian")) return BuiltinTime::kTime;
    if (reads_as("xing", "qi")) return BuiltinTime::kWeek;
    return BuiltinTime::kNone;
}

std::vector<std::string> builtin_time_phrases(BuiltinTime kind, const std::tm& t) {
    const std::string y = std::to_string(t.tm_year + 1900);
    const std::string mm = two(t.tm_mon + 1);
    const std::string dd = two(t.tm_mday);
    if (kind == BuiltinTime::kDate) {
        return {y + "年" + std::to_string(t.tm_mon + 1) + "月" + std::to_string(t.tm_mday) + "日",
                y + "-" + mm + "-" + dd,
                y + "/" + mm + "/" + dd,
                y + "." + mm + "." + dd,
                y + mm + dd,
                chinese_digits(y) + "年" + chinese_number(t.tm_mon + 1) + "月" +
                    chinese_number(t.tm_mday) + "日"};
    }
    if (kind == BuiltinTime::kTime) {
        const std::string noon = t.tm_hour < 12 ? "上午" : "下午";
        return {two(t.tm_hour) + ":" + two(t.tm_min),
                two(t.tm_hour) + ":" + two(t.tm_min) + ":" + two(t.tm_sec),
                noon + std::to_string(half_hour(t)) + ":" + two(t.tm_min),
                noon + chinese_number(half_hour(t)) + "点" +
                    (t.tm_min == 0 ? std::string("整") : chinese_two_digits(t.tm_min) + "分")};
    }
    if (kind == BuiltinTime::kWeek) {
        const std::string day = kWeekdays[t.tm_wday % 7];
        return {"星期" + day, "周" + day, "礼拜" + day};
    }
    return {};
}

CustomPhraseTable::CustomPhraseTable(std::vector<CustomPhrase> phrases) {
    for (CustomPhrase& phrase : phrases) {
        if (!is_valid_custom_phrase_code(phrase.code) || phrase.text.empty()) continue;
        by_code_[phrase.code].push_back(std::move(phrase));
        ++count_;
    }
}

const std::vector<CustomPhrase>* CustomPhraseTable::lookup(const std::string& code) const {
    const auto found = by_code_.find(code);
    return found == by_code_.end() ? nullptr : &found->second;
}

namespace {
std::mutex g_shared_mutex;
std::shared_ptr<const CustomPhraseTable> g_shared;
}  // namespace

std::shared_ptr<const CustomPhraseTable> CustomPhraseTable::shared() {
    std::lock_guard<std::mutex> lock(g_shared_mutex);
    return g_shared;
}

void CustomPhraseTable::set_shared(std::shared_ptr<const CustomPhraseTable> table) {
    std::lock_guard<std::mutex> lock(g_shared_mutex);
    g_shared = std::move(table);
}

}  // namespace cxxime
