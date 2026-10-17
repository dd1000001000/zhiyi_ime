// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// A word trigram language model next to the word choice (laya_rerank.cc): ln P(candidate | the
// text before it), the candidate split into the model's words as the model likes best. The file
// (.ngram, written by bench/ngram_prune.py from an ARPA model) is mapped, not read onto the heap:
//
//   char[8] "ZYNGRAM1"; u32 n1, n2, n3, 0                  (little endian, arrays 8-byte aligned)
//   u32 word_off[n1 + 1]; u8 words[]       the words' UTF-8, sorted bytewise (id = rank)
//   i16 p1[n1]; i16 b1[n1]                 log10 P and backoff, * 1/2048
//   u32 bi_start[n1 + 1]; u32 bi_w[n2]; i16 bi_p[n2]; i16 bi_b[n2]     bigrams by (v, w)
//   u32 tri_start[n2 + 1]; u32 tri_w[n3]; i16 tri_p[n3]                trigrams by (bigram, w)
//
// Backoff as ARPA has it; no <s> (a text starts from no history).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cxxime/mapped_file.h>

namespace laya {

class Ngram {
 public:
  // The characters of the context read (its last line's), the longest word looked up, and the
  // states kept per position (bench ngram_lm.py: CTX, MAX_WORD, BEAM).
  static constexpr size_t kContextChars = 12;
  static constexpr size_t kMaxWordChars = 8;
  static constexpr size_t kBeam = 8;

  // False (nothing mapped) when the file is missing or not a .ngram file.
  bool open(const std::string& path);
  bool is_open() const { return words_ != nullptr; }
  size_t bytes() const { return file_.size(); }

  // ln P(text | context) per text: the best split of context + text less the best of the
  // context alone.
  std::vector<double> score(const std::string& context, const std::vector<std::string>& texts) const;

  // The word's id, -1 for none.
  int64_t word(std::string_view text) const;
  // log10 P(w | v) and P(w | u v); u or v -1 for no history.
  double log10_p(int64_t v, int64_t w) const;
  double log10_p(int64_t u, int64_t v, int64_t w) const;

 private:
  struct State {
    int64_t u, v;
    double score;
  };
  int64_t bigram(int64_t v, int64_t w) const;  // index, -1 for none
  // The states at the end of chars[0, n) run from `states` at position `start` (beam).
  std::vector<State> run(const std::vector<std::string>& chars, size_t start, std::vector<State> states) const;

  cxxime::MappedFile file_;
  uint32_t n1_ = 0, n2_ = 0, n3_ = 0;
  const uint32_t* word_off_ = nullptr;
  const char* words_ = nullptr;
  const int16_t *p1_ = nullptr, *b1_ = nullptr;
  const uint32_t *bi_start_ = nullptr, *bi_w_ = nullptr;
  const int16_t *bi_p_ = nullptr, *bi_b_ = nullptr;
  const uint32_t *tri_start_ = nullptr, *tri_w_ = nullptr;
  const int16_t* tri_p_ = nullptr;
  int64_t unk_ = -1;
};

}  // namespace laya
