// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// Next words after the text before the caret, written by the next-word decoder of the one graph
// (ml-flash-next, bench next_word/export_flash_next.py; Reranker::has_next). The reference is
// dev/release_flashnext_tools/onnx_eval.py OnnxNext.generate / score_words (= next_flash.FlashDec):
//
//   context block  [CLS] (language tag) last context_tokens tokens of the context [SEP] (the mask
//                  token written as a space; spaced languages without trailing spaces): the word
//                  choice's own block, so its cache usually serves both
//   search         beam search over the decoder's rows (next_rows.json): Chinese, Traditional and
//                  Japanese any allowed row, then rows or the end, a word kept with 2+ characters;
//                  English, Korean (and Vietnamese) a word start, then pieces or the end; the end
//                  never first; at most max_word_tokens tokens; hypotheses writing the same text
//                  summed; stop once k words are found and no open hypothesis is above the k-th
//   n-gram         (Chinese) the n-gram's next words (Ngram::next_words) scored by the decoder
//                  (teacher forced, within the language's rows) and mixed with the decoder's own:
//                  ln P_decoder + weight * ln P_ngram (bench next_flash.evaluate "with ngram")
//
// next_rows.json: each row's token id and piece, the languages' tags and masks (allow / first /
// cont row lists), the end row, max_word_tokens, context_tokens, the CJK languages.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace laya {

class Reranker;
class Ngram;

struct NextWord {
  std::string text;
  double ln_p = 0.0;  // the decoder's ln P of the word (then the end); the order may also use the n-gram
};

class NextWords {
 public:
  // The model must have the decoder (has_next). Throws when next_rows.json is missing or invalid.
  NextWords(const Reranker& model, const std::string& rows_json);
  ~NextWords();

  bool has_language(const std::string& lang) const { return langs_.count(lang) != 0; }

  // The k most likely next words after `context` in `lang` (zh, en, hant, ja, ko, vi), best first:
  // the decoder's beam search alone. Empty when the model fails. Thread-safe.
  std::vector<NextWord> predict(const std::string& context, const std::string& lang, size_t k, size_t beam) const;

  // ln P(word, then the end | context) of each word under the decoder, teacher forced, every step
  // within the language's rows; NaN for a word it cannot write (a token outside its rows, too
  // long). Thread-safe.
  std::vector<double> score(const std::string& context, const std::string& lang,
                            const std::vector<std::string>& words) const;

  // predict() with the n-gram's next words mixed in (when `ngram` is open and weight > 0; only
  // for the n-gram's language, Simplified Chinese): the decoder's `pool` words (beam `beam`) and
  // the n-gram's `ngram_pool` words, each word's score ln P_decoder + weight * ln P_ngram (a word
  // missing on one side: that side's lowest less 1), the k best. Words whose decoder probability
  // is below min_probability are left out (also below k then), but not the n-gram's own words.
  std::vector<NextWord> predict_mixed(const std::string& context, const std::string& lang, size_t k, size_t beam,
                                      const Ngram* ngram, double weight, size_t pool, size_t ngram_pool,
                                      double min_probability) const;

  // The block the decoder reads (exposed for tests).
  std::vector<int64_t> block(const std::string& context, const std::string& lang) const;

 private:
  struct Lang {
    bool cjk = false;
    std::vector<int64_t> tag;                     // its tag's token ids (none for zh, en)
    std::vector<uint8_t> allow, first, cont;      // per row
    std::vector<int32_t> allow_rows, first_rows, cont_rows;  // the same as row lists
  };
  std::string text(const std::vector<int32_t>& rows, const Lang& lang, const std::string& code) const;
  // A whole word's rows as it follows the context (bench Enc.word); empty when it cannot be written.
  std::vector<int32_t> word_rows(const std::string& word, const Lang& lang, const std::string& code) const;

  const Reranker& model_;
  std::vector<int64_t> tokens_;       // row -> token id
  std::vector<std::string> pieces_;   // row -> the tokenizer's piece
  std::vector<std::string> body_;     // row -> the piece without a leading "▁" (CJK text)
  std::unordered_map<int64_t, int32_t> row_of_token_;
  std::unordered_map<std::string, Lang> langs_;
  int32_t end_ = 1;
  size_t max_word_tokens_ = 6;
  size_t context_tokens_ = 128;
};

}  // namespace laya
