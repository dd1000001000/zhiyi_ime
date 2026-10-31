// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// U mode and V mode (issue #41, docs/uv-mode.md), as in Microsoft Pinyin. In full pinyin a u or
// v typed first starts them (no syllable starts with either); in shuangpin and Wubi, Shift+U and
// Shift+V.
//
// V mode turns what follows the v into text:
//   v123          一百二十三 壹佰贰拾叁 一二三 壹贰叁 一百二十三元 壹佰贰拾叁元整 CXXIII
//   v123.45       一百二十三元四角五分 壹佰贰拾叁元肆角伍分 一百二十三点四五 ...
//   v2026.10.31   2026年10月31日 二〇二六年十月三十一日 ... 农历二〇二六年九月廿二 星期六
//   v12:34        十二时三十四分 12时34分 十二分三十四秒 12分34秒
//   v1+2*3        7  1+2*3=7      (+ - * / ^ % ! ( ), sqrt sin log ..., pi e)
//   vu6d3e        派 (a Unicode code point)   vgbc5c9  派 (a GB 18030 code)
// Its digits are input, so its candidates are chosen with a, b, c ...
//
// U mode finds a character by its strokes and parts, showing its readings:
//   uspnp         少 ...          h 横  s 竖  p 撇  n 捺/点  z 折
//   umumu         林 ...          the parts' readings: 木 木
//   uqips         氚 ...          parts and strokes mixed: 气, then a part starting 丿丨
//   uudw          ℃ ℉ ...         uu + a symbol group (as the / menu)
#ifndef CXXIME_UV_MODE_H_
#define CXXIME_UV_MODE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cxxime {

inline constexpr char kUModePrefix = 'u';
inline constexpr char kVModePrefix = 'v';

struct UvCandidate {
    std::string text;
    std::string comment;  // shown after the text (U mode: the readings)
};

// ---- V mode ----

// The candidates for what follows the v ("123", "2026.10.31", "1+2*3", "u6d3e").
std::vector<UvCandidate> v_mode_candidates(std::string_view input);
// Whether the letters choose candidates now (labels a, b, c ...): not while a code (u..., gb...)
// or a function name is being typed.
bool v_mode_letters_choose(std::string_view input);
// The keys V mode takes as input besides letters and digits.
bool is_v_mode_symbol(char ch);

// Parts of V mode, for tests.
std::string chinese_number(std::string_view digits, bool upper);  // "123" -> 一百二十三
std::string chinese_digits(std::string_view digits, bool upper);  // "123" -> 一二三
std::string roman_number(int value);                              // 1 .. 3999
// The lunar date ("农历二〇二六年九月廿二"), empty when Windows has no ICU (before 1903).
std::string lunar_date(int year, int month, int day);
// Evaluates a formula; false when it does not parse or has no finite value.
bool evaluate_formula(std::string_view formula, double* value);
std::string format_number(double value);

// ---- U mode ----

// The characters of data/uv_chars.tsv: strokes, readings, frequency and parts.
class UModeTable {
public:
    // Views into the file's text, kept by the table.
    struct Entry {
        std::string_view text;
        std::string_view strokes;   // "zpnp,spnp" (stroke orders differ by region)
        std::string_view readings;  // "shǎo,shào"
        std::string_view plain;     // "shao"
        int frequency = 0;
    };

    UModeTable() = default;
    UModeTable(const UModeTable&) = delete;  // entries point into text_
    UModeTable& operator=(const UModeTable&) = delete;

    bool load(const std::string& path);
    bool parse(std::string text);
    bool empty() const { return entries_.empty(); }

    // The characters for what follows the u ("spnp", "mumu", "qi'ps"), best first.
    std::vector<int> lookup(std::string_view input, std::size_t limit) const;
    const Entry& entry(int index) const { return entries_[static_cast<std::size_t>(index)]; }

    // data/uv_chars.tsv, read the first time it is asked for.
    static std::shared_ptr<const UModeTable> shared();
    static void set_shared(std::shared_ptr<const UModeTable> table);

private:
    struct Token {
        std::string_view text;
        bool strokes = false;  // h s p n z; else a part's reading
        bool last = false;     // the reading may be typed in part
    };

    std::vector<int> lookup_strokes(std::string_view strokes, std::size_t limit) const;
    std::vector<int> lookup_parts(std::string_view input, std::size_t limit) const;
    bool part_matches(int part, const Token& token) const;

    std::string text_;
    std::vector<Entry> entries_;
    // Each character's splits into parts (indexes into entries_).
    std::unordered_map<int, std::vector<std::vector<int>>> splits_;
    // (character, split) by the split's first part.
    std::unordered_map<int, std::vector<std::pair<int, int>>> by_first_part_;
    std::vector<int> parts_;  // every entry that is some character's part
    std::vector<std::string> syllables_;  // the parts' readings, for splitting the input
};

// The U mode candidates for what follows the u (not the uu symbols, which the / menu's table
// gives).
std::vector<UvCandidate> u_mode_candidates(const UModeTable& table, std::string_view input,
                                           std::size_t limit);

}  // namespace cxxime

#endif  // CXXIME_UV_MODE_H_
