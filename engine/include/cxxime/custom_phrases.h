// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Custom phrases (issue #40, docs/custom-phrases.md): letters that bring up a text at a chosen
// place on the page ("dh" → a phone number, "wjx" → ★ ☆), kept in the user's phrases.ini in
// Sogou Pinyin's format, so a Sogou user's file works as it is:
//
//   ; a comment
//   dh,1=13901234567
//   wjx,1=★
//   wjx,2=☆
//   dz,1=
//   北京市西城区1号院
//   (the lines up to the next "code,place=" belong to the phrase)
//   rq,1=#$year年$month月$day日
//
// A phrase starting with # is filled with the date and time ($year ... $second_cn, as Sogou).
// Pinyin read as 日期 / 时间 / 星期 (riqi, rq, shijian, shij, sj, xq ...) brings up the date, time
// and weekday built in, unless the user has a phrase for the same letters; the / menu has them
// too. Committing one learns nothing (CandidateSource::kPhrase).
#ifndef CXXIME_CUSTOM_PHRASES_H_
#define CXXIME_CUSTOM_PHRASES_H_

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cxxime {

inline constexpr int kMaxCustomPhrasePosition = 9;
inline constexpr std::size_t kMaxCustomPhrases = 100000;
inline constexpr std::size_t kMaxCustomPhraseBytes = 30000 * 3;  // 30,000 Chinese characters
inline constexpr std::size_t kMaxCustomPhraseCodeBytes = 32;

struct CustomPhrase {
    std::string code;  // lowercase letters
    int position = 1;  // 1 .. kMaxCustomPhrasePosition
    std::string text;  // UTF-8; lines joined by "\n"; # first: filled with the date and time
};

// The bytes of a phrase file as UTF-8: UTF-8 (with or without a BOM), UTF-16LE with its BOM
// (as Sogou writes it), else GBK.
std::string decode_phrase_file(std::string_view bytes);
// Sogou's phrases.ini, decoded or not; entries that do not parse are skipped.
std::vector<CustomPhrase> parse_custom_phrases(std::string_view bytes);
// Rime's custom_phrase.txt (text<Tab>code<Tab>weight): each code's phrases placed 1, 2, ... by
// weight, highest first.
std::vector<CustomPhrase> parse_rime_custom_phrases(std::string_view bytes);
// phrases.ini text (UTF-8, CRLF), the entries in order.
std::string serialize_custom_phrases(const std::vector<CustomPhrase>& phrases);
// Rime's custom_phrase.txt (UTF-8): text<Tab>code<Tab>weight, a place's weight 10 - place so
// that parse_rime_custom_phrases gives the places back. Rime has no multi-line phrases nor
// date variables: those are left out and counted in `skipped`.
std::string serialize_rime_custom_phrases(const std::vector<CustomPhrase>& phrases,
                                          std::size_t* skipped = nullptr);
// Whether `code` can be a phrase's letters.
bool is_valid_custom_phrase_code(std::string_view code);

// `text` with the date and time filled in when it starts with # (the # dropped); else as it is.
std::string expand_custom_phrase(const std::string& text, const std::tm& now);
// The built-in date, time and weekday.
enum class BuiltinTime { kNone, kDate, kTime, kWeek };
// Which one the pinyin letters read as: 日期 ri qi, 时间 shi jian, 星期 xing qi, each syllable
// typed in full or in part (riqi, rq, riq, shijian, shij, sj, xingq, xq); kNone otherwise.
BuiltinTime builtin_time_for(const std::string& code);
// Its texts, in their order (2026年10月31日, 2026-10-31 ...).
std::vector<std::string> builtin_time_phrases(BuiltinTime kind, const std::tm& now);

// The phrases by their letters, in file order. One table is shared by every session
// (set by the server when phrases.ini is read or changed).
class CustomPhraseTable {
public:
    explicit CustomPhraseTable(std::vector<CustomPhrase> phrases = {});
    const std::vector<CustomPhrase>* lookup(const std::string& code) const;
    std::size_t size() const { return count_; }

    static std::shared_ptr<const CustomPhraseTable> shared();
    static void set_shared(std::shared_ptr<const CustomPhraseTable> table);

private:
    std::map<std::string, std::vector<CustomPhrase>> by_code_;
    std::size_t count_ = 0;
};

}  // namespace cxxime

#endif  // CXXIME_CUSTOM_PHRASES_H_
