// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// V mode (cxxime/uv_mode.h): numbers in Chinese, dates, times, formulas and character codes.

#include <cxxime/uv_mode.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cxxime {
namespace {

const char* const kLowerDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
const char* const kUpperDigits[] = {"零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"};
// Read one by one, 0 is 〇 in small digits (二〇二六年) and 零 in capitals.
const char* const kLowerSpelled[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
const char* const kLowerUnits[] = {"千", "百", "十", ""};
const char* const kUpperUnits[] = {"仟", "佰", "拾", ""};
const char* const kGroupUnits[] = {"", "万", "亿", "万亿"};
constexpr std::size_t kMaxReadDigits = 16;

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int to_int(std::string_view digits) {
    int value = 0;
    for (char c : digits) value = value * 10 + (c - '0');
    return value;
}

bool is_letter(char c) { return c >= 'a' && c <= 'z'; }

bool is_hex(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string utf8_of(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string utf8_of(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

void add(std::vector<UvCandidate>& out, std::string text, std::string comment = {}) {
    if (text.empty()) return;
    for (const UvCandidate& c : out) {
        if (c.text == text) return;
    }
    out.push_back({std::move(text), std::move(comment)});
}

// ---- numbers ----

std::string money(std::string_view whole, std::string_view fraction, bool upper) {
    const char* const* digits = upper ? kUpperDigits : kLowerDigits;
    std::string trimmed(whole);
    trimmed.erase(0, (std::min)(trimmed.find_first_not_of('0'), trimmed.size()));
    const int jiao = fraction.size() > 0 ? fraction[0] - '0' : 0;
    const int fen = fraction.size() > 1 ? fraction[1] - '0' : 0;
    std::string out;
    if (!trimmed.empty()) out = chinese_number(trimmed, upper) + "元";
    if (jiao == 0 && fen == 0) {
        if (out.empty()) out = std::string(digits[0]) + "元";
        // Capitals end with 整 as on a cheque (壹佰贰拾叁元整).
        return upper ? out + "整" : out;
    }
    if (jiao > 0) {
        out += std::string(digits[jiao]) + "角";
    } else if (!out.empty()) {
        out += digits[0];
    }
    if (fen > 0) out += std::string(digits[fen]) + "分";
    else if (upper) out += "整";
    return out;
}

std::string decimal_reading(std::string_view whole, std::string_view fraction, bool upper) {
    const char* const* digits = upper ? kUpperDigits : kLowerDigits;
    std::string out = chinese_number(whole, upper) + "点";
    for (char c : fraction) out += digits[c - '0'];
    return out;
}

void add_number(std::vector<UvCandidate>& out, std::string_view whole, std::string_view fraction) {
    const bool readable = whole.size() <= kMaxReadDigits;
    if (fraction.empty()) {
        if (readable) {
            add(out, chinese_number(whole, false));
            add(out, chinese_number(whole, true));
        }
        add(out, chinese_digits(whole, false));
        add(out, chinese_digits(whole, true));
        if (readable) {
            add(out, money(whole, "", false));
            add(out, money(whole, "", true));
        }
        if (whole.size() <= 4) add(out, roman_number(to_int(whole)));
        return;
    }
    if (readable && fraction.size() <= 2) {
        add(out, money(whole, fraction, false));
        add(out, money(whole, fraction, true));
    }
    if (readable) {
        add(out, decimal_reading(whole, fraction, false));
        add(out, decimal_reading(whole, fraction, true));
    }
    add(out, chinese_digits(whole, false) + "点" + chinese_digits(fraction, false));
    add(out, chinese_digits(whole, true) + "点" + chinese_digits(fraction, true));
}

// ---- dates and times ----

bool leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : kDays[month - 1];
}

// 0 is Sunday.
int weekday(int year, int month, int day) {
    static const int kOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    return (year + year / 4 - year / 100 + year / 400 + kOffsets[month - 1] + day) % 7;
}

std::vector<std::string_view> split(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = s.find(separator, start);
        parts.push_back(s.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

std::string number_of(int value, bool upper) { return chinese_number(std::to_string(value), upper); }

// "2026.10.31", "2026-10-31", "2026/10/31", "10.31" (month and day), "2026.10" (year and month).
void add_dates(std::vector<UvCandidate>& out, std::string_view input) {
    char separator = 0;
    for (char c : {'.', '-', '/'}) {
        if (input.find(c) != std::string_view::npos) {
            if (separator) return;
            separator = c;
        }
    }
    if (!separator) return;
    const std::vector<std::string_view> parts = split(input, separator);
    for (std::string_view part : parts) {
        if (!all_digits(part)) return;
    }
    int year = 0, month = 0, day = 0;
    if (parts.size() == 3 && parts[0].size() == 4 && parts[1].size() <= 2 && parts[2].size() <= 2) {
        year = to_int(parts[0]);
        month = to_int(parts[1]);
        day = to_int(parts[2]);
    } else if (parts.size() == 2 && parts[0].size() == 4 && parts[1].size() <= 2) {
        year = to_int(parts[0]);
        month = to_int(parts[1]);
    } else if (parts.size() == 2 && parts[0].size() <= 2 && parts[1].size() <= 2) {
        month = to_int(parts[0]);
        day = to_int(parts[1]);
    } else {
        return;
    }
    if (month < 1 || month > 12) return;
    if (day && day > days_in_month(year ? year : 2000, month)) return;
    if (parts.size() == 2 && !year && day == 0) return;
    const std::string y = std::to_string(year);
    if (year && day) {
        add(out, y + "年" + std::to_string(month) + "月" + std::to_string(day) + "日");
        add(out, chinese_digits(y, false) + "年" + number_of(month, false) + "月" +
                     number_of(day, false) + "日");
        add(out, chinese_digits(y, true) + "年" + number_of(month, true) + "月" +
                     number_of(day, true) + "日");
        add(out, lunar_date(year, month, day));
        static const char* const kWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        add(out, std::string("星期") + kWeekdays[weekday(year, month, day)]);
    } else if (year) {
        add(out, y + "年" + std::to_string(month) + "月");
        add(out, chinese_digits(y, false) + "年" + number_of(month, false) + "月");
        add(out, chinese_digits(y, true) + "年" + number_of(month, true) + "月");
    } else {
        add(out, std::to_string(month) + "月" + std::to_string(day) + "日");
        add(out, number_of(month, false) + "月" + number_of(day, false) + "日");
        add(out, number_of(month, true) + "月" + number_of(day, true) + "日");
    }
}

// 5 -> 零五 (after an hour or minute), 34 -> 三十四.
std::string clock_part(int value) {
    return value < 10 && value > 0 ? std::string("零") + kLowerDigits[value] : number_of(value, false);
}

// "12:34" (hours and minutes, or minutes and seconds), "12:34:56".
void add_times(std::vector<UvCandidate>& out, std::string_view input) {
    const std::vector<std::string_view> parts = split(input, ':');
    if (parts.size() < 2 || parts.size() > 3) return;
    for (std::string_view part : parts) {
        if (!all_digits(part) || part.size() > 2) return;
    }
    const int a = to_int(parts[0]);
    const int b = to_int(parts[1]);
    const std::string as(parts[0]), bs(parts[1]);
    if (parts.size() == 3) {
        const int c = to_int(parts[2]);
        if (a > 24 || b > 59 || c > 59) return;
        add(out, number_of(a, false) + "时" + clock_part(b) + "分" + clock_part(c) + "秒");
        add(out, as + "时" + bs + "分" + std::string(parts[2]) + "秒");
        return;
    }
    if (b > 59) return;
    if (a <= 24) {
        add(out, number_of(a, false) + "时" + (b ? clock_part(b) + "分" : std::string("整")));
        add(out, as + "时" + bs + "分");
    }
    if (a <= 59) {
        add(out, number_of(a, false) + "分" + (b ? clock_part(b) + "秒" : std::string("整")));
        add(out, as + "分" + bs + "秒");
    }
}

// ---- codes ----

void add_codes(std::vector<UvCandidate>& out, std::string_view input) {
    if (input.size() >= 2 && input[0] == 'u' && input.size() <= 7 && is_hex(input.substr(1))) {
        const unsigned long cp = std::strtoul(std::string(input.substr(1)).c_str(), nullptr, 16);
        if (cp >= 0x20 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF) && !(cp >= 0x7F && cp < 0xA0)) {
            add(out, utf8_of(static_cast<char32_t>(cp)));
        }
        return;
    }
    if (input.size() >= 4 && input.substr(0, 2) == "gb" && input.size() % 2 == 0 &&
        input.size() <= 10 && is_hex(input.substr(2))) {
        std::string bytes;
        for (std::size_t i = 2; i < input.size(); i += 2) {
            bytes += static_cast<char>(std::strtoul(std::string(input.substr(i, 2)).c_str(), nullptr, 16));
        }
        wchar_t wide[4] = {};
        const int n = MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, bytes.data(),
                                          static_cast<int>(bytes.size()), wide, 4);
        // One character (two UTF-16 units for one beyond the BMP).
        const bool one = n == 1 || (n == 2 && IS_HIGH_SURROGATE(wide[0]));
        if (one && wide[0] >= 0x20) add(out, utf8_of(std::wstring(wide, static_cast<std::size_t>(n))));
    }
}

// ---- formulas ----

class FormulaParser {
public:
    explicit FormulaParser(std::string_view text) : text_(text) {}

    bool parse(double* value) {
        if (!expression(value)) return false;
        return pos_ == text_.size() && std::isfinite(*value);
    }

private:
    bool peek(char c) const { return pos_ < text_.size() && text_[pos_] == c; }
    bool take(char c) {
        if (!peek(c)) return false;
        ++pos_;
        return true;
    }

    bool expression(double* value) {
        if (!term(value)) return false;
        while (peek('+') || peek('-')) {
            const char op = text_[pos_++];
            double right = 0;
            if (!term(&right)) return false;
            *value = op == '+' ? *value + right : *value - right;
        }
        return true;
    }

    bool term(double* value) {
        if (!unary(value)) return false;
        while (peek('*') || peek('/')) {
            const char op = text_[pos_++];
            double right = 0;
            if (!unary(&right)) return false;
            if (op == '/' && right == 0) return false;
            *value = op == '*' ? *value * right : *value / right;
        }
        return true;
    }

    bool unary(double* value) {
        if (take('-')) {
            if (!unary(value)) return false;
            *value = -*value;
            return true;
        }
        if (take('+')) return unary(value);
        return power(value);
    }

    bool power(double* value) {
        if (!postfix(value)) return false;
        if (take('^')) {
            double exponent = 0;
            if (!unary(&exponent)) return false;
            *value = std::pow(*value, exponent);
        }
        return true;
    }

    bool postfix(double* value) {
        if (!primary(value)) return false;
        while (true) {
            if (take('!')) {
                if (*value < 0 || *value > 170 || *value != std::floor(*value)) return false;
                double f = 1;
                for (int i = 2; i <= static_cast<int>(*value); ++i) f *= i;
                *value = f;
            } else if (take('%')) {
                *value /= 100;
            } else {
                return true;
            }
        }
    }

    bool number(double* value) {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.')) ++pos_;
        const std::string digits(text_.substr(start, pos_ - start));
        if (digits.empty() || digits == "." || std::count(digits.begin(), digits.end(), '.') > 1) return false;
        *value = std::strtod(digits.c_str(), nullptr);
        return true;
    }

    bool primary(double* value) {
        if (take('(')) {
            if (!expression(value)) return false;
            return take(')');
        }
        if (pos_ < text_.size() && is_letter(text_[pos_])) {
            const std::size_t start = pos_;
            while (pos_ < text_.size() && is_letter(text_[pos_])) ++pos_;
            const std::string name(text_.substr(start, pos_ - start));
            if (name == "pi") {
                *value = 3.14159265358979323846;
                return true;
            }
            if (name == "e") {
                *value = 2.71828182845904523536;
                return true;
            }
            if (!take('(')) return false;
            std::vector<double> args;
            if (!take(')')) {
                while (true) {
                    double arg = 0;
                    if (!expression(&arg)) return false;
                    args.push_back(arg);
                    if (take(')')) break;
                    if (!take(',')) return false;
                }
            }
            return call(name, args, value);
        }
        return number(value);
    }

    static bool call(const std::string& name, const std::vector<double>& args, double* value) {
        constexpr double kDegree = 3.14159265358979323846 / 180;
        if (args.empty()) return false;
        const double x = args[0];
        if (args.size() == 1) {
            // Angles in degrees, as on a desk calculator: sin(30) = 0.5.
            if (name == "sqrt") *value = std::sqrt(x);
            else if (name == "cbrt") *value = std::cbrt(x);
            else if (name == "abs") *value = std::fabs(x);
            else if (name == "sin") *value = std::sin(x * kDegree);
            else if (name == "cos") *value = std::cos(x * kDegree);
            else if (name == "tan") *value = std::tan(x * kDegree);
            else if (name == "cot") *value = 1 / std::tan(x * kDegree);
            else if (name == "asin" || name == "arcsin") *value = std::asin(x) / kDegree;
            else if (name == "acos" || name == "arccos") *value = std::acos(x) / kDegree;
            else if (name == "atan" || name == "arctan") *value = std::atan(x) / kDegree;
            else if (name == "ln") *value = std::log(x);
            else if (name == "log" || name == "lg") *value = std::log10(x);
            else if (name == "exp") *value = std::exp(x);
            else if (name == "floor") *value = std::floor(x);
            else if (name == "ceil") *value = std::ceil(x);
            else if (name == "round") *value = std::round(x);
            else if (name != "sum" && name != "avg" && name != "max" && name != "min") return false;
            else *value = x;
            // sin(180) is 1.2e-16: exact where a degree angle gives a round value.
            if (std::fabs(*value) < 1e-12) *value = 0;
            return std::isfinite(*value);
        }
        if (name == "log" && args.size() == 2) {
            *value = std::log(args[1]) / std::log(args[0]);
            return std::isfinite(*value);
        }
        if (name == "pow" && args.size() == 2) {
            *value = std::pow(args[0], args[1]);
            return std::isfinite(*value);
        }
        double sum = 0;
        for (double a : args) sum += a;
        const double mean = sum / static_cast<double>(args.size());
        if (name == "sum") *value = sum;
        else if (name == "avg") *value = mean;
        else if (name == "max") *value = *std::max_element(args.begin(), args.end());
        else if (name == "min") *value = *std::min_element(args.begin(), args.end());
        else if (name == "var" || name == "stdev" || name == "std") {
            double squares = 0;
            for (double a : args) squares += (a - mean) * (a - mean);
            const double variance = squares / static_cast<double>(args.size() - 1);  // a sample's
            *value = name == "var" ? variance : std::sqrt(variance);
        } else {
            return false;
        }
        return std::isfinite(*value);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

bool has_operator(std::string_view input) {
    for (char c : input) {
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '!' || c == '%' ||
            c == '(' || is_letter(c))
            return true;
    }
    return false;
}

}  // namespace

std::string chinese_number(std::string_view digits, bool upper) {
    std::string trimmed(digits);
    trimmed.erase(0, (std::min)(trimmed.find_first_not_of('0'), trimmed.size()));
    const char* const* names = upper ? kUpperDigits : kLowerDigits;
    if (trimmed.empty()) return names[0];
    if (trimmed.size() > kMaxReadDigits) return {};
    const char* const* units = upper ? kUpperUnits : kLowerUnits;
    // Groups of four from the right: 万, 亿, 万亿.
    const std::size_t groups = (trimmed.size() + 3) / 4;
    trimmed.insert(0, groups * 4 - trimmed.size(), '0');
    std::string out;
    bool any = false;
    bool zero = false;
    for (std::size_t g = 0; g < groups; ++g) {
        const std::string_view group = std::string_view(trimmed).substr(g * 4, 4);
        if (group == "0000") {
            if (any) zero = true;
            continue;
        }
        for (int i = 0; i < 4; ++i) {
            const int d = group[static_cast<std::size_t>(i)] - '0';
            if (d == 0) {
                if (any) zero = true;
                continue;
            }
            if (zero) out += names[0];
            zero = false;
            out += names[d];
            out += units[i];
            any = true;
        }
        out += kGroupUnits[groups - 1 - g];
    }
    // 十, 十五, 十万: no 一 before a leading ten in small digits (壹拾 in capitals).
    const std::string leading_ten = std::string(names[1]) + units[2];
    if (!upper && out.compare(0, leading_ten.size(), leading_ten) == 0) out.erase(0, std::strlen(names[1]));
    return out;
}

std::string chinese_digits(std::string_view digits, bool upper) {
    std::string out;
    for (char c : digits) {
        if (c < '0' || c > '9') return {};
        out += (upper ? kUpperDigits : kLowerSpelled)[c - '0'];
    }
    return out;
}

std::string roman_number(int value) {
    if (value < 1 || value > 3999) return {};
    static const struct { int value; const char* text; } kNumerals[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
        {50, "L"},   {40, "XL"},  {10, "X"},  {9, "IX"},   {5, "V"},   {4, "IV"}, {1, "I"}};
    std::string out;
    for (const auto& numeral : kNumerals) {
        while (value >= numeral.value) {
            out += numeral.text;
            value -= numeral.value;
        }
    }
    return out;
}

// The Chinese calendar of ICU, which Windows has had since 10 version 1903 (icu.dll).
std::string lunar_date(int year, int month, int day) {
    using UCalendar = void;
    using OpenFn = UCalendar* (*)(const wchar_t*, int32_t, const char*, int, int*);
    using CloseFn = void (*)(UCalendar*);
    using SetMillisFn = void (*)(UCalendar*, double, int*);
    using GetFn = int32_t (*)(const UCalendar*, int, int*);
    static const struct Icu {
        OpenFn open = nullptr;
        CloseFn close = nullptr;
        SetMillisFn set_millis = nullptr;
        GetFn get = nullptr;
        Icu() {
            HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!icu) return;
            open = reinterpret_cast<OpenFn>(GetProcAddress(icu, "ucal_open"));
            close = reinterpret_cast<CloseFn>(GetProcAddress(icu, "ucal_close"));
            set_millis = reinterpret_cast<SetMillisFn>(GetProcAddress(icu, "ucal_setMillis"));
            get = reinterpret_cast<GetFn>(GetProcAddress(icu, "ucal_get"));
        }
    } icu;
    if (!icu.open || !icu.close || !icu.set_millis || !icu.get) return {};
    int status = 0;
    UCalendar* calendar = icu.open(L"UTC", 3, "zh_CN@calendar=chinese", 0, &status);
    if (!calendar || status > 0) return {};
    // Noon of the day (UTC): days since 1970-01-01.
    const int y = month <= 2 ? year - 1 : year;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const double days = era * 146097.0 + doe - 719468;
    icu.set_millis(calendar, (days + 0.5) * 86400000.0, &status);
    enum { kMonth = 2, kDate = 5, kExtendedYear = 19, kIsLeapMonth = 22 };
    const int lunar_year = icu.get(calendar, kExtendedYear, &status) - 2637;
    const int lunar_month = icu.get(calendar, kMonth, &status) + 1;
    const bool leap = icu.get(calendar, kIsLeapMonth, &status) != 0;
    const int lunar_day = icu.get(calendar, kDate, &status);
    icu.close(calendar);
    if (status > 0 || lunar_month < 1 || lunar_month > 12 || lunar_day < 1 || lunar_day > 30) return {};
    static const char* const kMonths[] = {"正", "二", "三", "四", "五", "六",
                                          "七", "八", "九", "十", "冬", "腊"};
    static const char* const kTens[] = {"初", "十", "廿", "三"};
    std::string day_name;
    if (lunar_day == 10) day_name = "初十";
    else if (lunar_day == 20) day_name = "二十";
    else if (lunar_day == 30) day_name = "三十";
    else day_name = std::string(kTens[lunar_day / 10]) + kLowerDigits[lunar_day % 10];
    return "农历" + chinese_digits(std::to_string(lunar_year), false) + "年" + (leap ? "闰" : "") +
           kMonths[lunar_month - 1] + "月" + day_name;
}

bool evaluate_formula(std::string_view formula, double* value) {
    return FormulaParser(formula).parse(value);
}

std::string format_number(double value) {
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.0f", value);
        return buffer;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.10g", value);
    return buffer;
}

bool is_v_mode_symbol(char ch) {
    return std::strchr(".+-*/^%!(),:", ch) != nullptr && ch != '\0';
}

bool v_mode_letters_choose(std::string_view input) {
    if (input.empty() || is_letter(input.back())) return false;
    // A code is hex: its letters are input.
    if (input[0] == 'u' || input.substr(0, 2) == "gb") return false;
    return true;
}

std::vector<UvCandidate> v_mode_candidates(std::string_view input) {
    std::vector<UvCandidate> out;
    if (input.empty()) {
        add(out, "v", "数字 日期 时间 公式 编码");
        return out;
    }
    add_codes(out, input);
    if (!out.empty()) return out;
    if (input.find(':') != std::string_view::npos) {
        add_times(out, input);
        return out;
    }
    add_dates(out, input);
    const std::size_t dot = input.find('.');
    if (all_digits(input)) {
        add_number(out, input, "");
    } else if (dot != std::string_view::npos && all_digits(input.substr(0, dot)) &&
               (dot + 1 == input.size() || all_digits(input.substr(dot + 1)))) {
        add_number(out, input.substr(0, dot), input.substr(dot + 1));
    }
    if (has_operator(input)) {
        double value = 0;
        if (evaluate_formula(input, &value)) {
            const std::string result = format_number(value);
            add(out, result);
            add(out, std::string(input) + "=" + result);
        }
    }
    return out;
}

}  // namespace cxxime
