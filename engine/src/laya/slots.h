// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// The unified model's typed-input slots (bench unified_common.py / p2c_common.py): each pinyin
// syllable or English letter the user typed is a [MASK] token plus a unit vector. This file is
// the text side: which unit each typed syllable is, and the model's vocabulary of units and
// characters (unified_vocab.json beside the model).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace laya {

struct Unit {
  char kind = 'f';   // 'f' a syllable typed in full, 'p' a prefix of one (an initial, or the
                     // unfinished last syllable), 'l' an English letter
  std::string text;  // the syllable, the prefix, or the letter
  bool operator==(const Unit& o) const { return kind == o.kind && text == o.text; }
};

// p2c_common.slot_units: the unit of each syllable as it was typed in `code` (letters only, no
// separators). Empty when the code does not spell the syllables that way (a fuzzy reading, or a
// shuangpin key). A syllable before the last one may only be abbreviated to its initial (one
// letter, or zh / ch / sh); the last one to any prefix.
std::vector<Unit> slot_units(const std::vector<std::string>& syllables, const std::string& code);

// English: one unit per typed letter (lowercased), at most max_slots.
std::vector<Unit> letter_units(const std::string& typed, size_t max_slots);

// The syllables of a candidate's reading ("zhong:guo" -> zhong, guo).
std::vector<std::string> split_syllables(std::string_view syllables);

// unified_vocab.json: the unit rows (Vocab's "none", f:..., p:... then one per letter), the
// characters the continuation head predicts and their readings.
class UnifiedVocab {
 public:
  explicit UnifiedVocab(const std::string& json_path);  // throws std::runtime_error

  int64_t unit_id(const Unit& unit) const;  // 0 when the vocabulary has no such unit
  size_t char_count() const { return chars_.size(); }
  const std::vector<std::string>& chars() const { return chars_; }
  int char_index(std::string_view utf8_char) const;  // -1 when not a character of the head
  // The characters a unit can spell (one byte per character, 1 = can), cached per unit.
  const std::vector<uint8_t>& allowed(int64_t unit_id) const;

  int max_slots() const { return max_slots_; }
  int max_context_tokens() const { return max_context_tokens_; }
  const std::string& context_prefix() const { return context_prefix_; }
  // The word-choice sequence: false = Laya's (instructions, candidates " c: c", then the context
  // and the slots); true = "ctx-first" (bench/qwen_teacher/student2.py): [CLS] the context's last
  // choice_context_tokens [SEP] slots [SEP] [MASK] " c" ... [SEP], each candidate at most
  // choice_candidate_tokens.
  bool context_first() const { return context_first_; }
  int choice_context_tokens() const { return choice_context_tokens_; }
  int choice_candidate_tokens() const { return choice_candidate_tokens_; }
  // The decoder graph beside the model (export_unified_dec.py), "" for a continuation head.
  const std::string& decoder_file() const { return decoder_file_; }
  // The same decoder as two graphs that keep a KV cache (export_unified_dec_kv.py): the
  // memory's cross-attention keys / values, and one character given the earlier ones'. "" without.
  const std::string& decoder_memory_file() const { return decoder_memory_file_; }
  const std::string& decoder_step_file() const { return decoder_step_file_; }

 private:
  std::unordered_map<std::string, int64_t> units_;  // "f:zhong" -> row
  std::string letters_;
  int64_t letter_base_ = 0;
  std::vector<std::string> chars_;
  std::unordered_map<std::string, int> char_index_;
  std::vector<std::vector<std::string>> readings_;  // per character
  std::vector<std::string> unit_names_;              // row -> "f:zhong"
  int max_slots_ = 24;
  int max_context_tokens_ = 160;
  std::string context_prefix_;
  bool context_first_ = false;
  int choice_context_tokens_ = 128;
  int choice_candidate_tokens_ = 16;
  std::string decoder_file_;
  std::string decoder_memory_file_;
  std::string decoder_step_file_;
  mutable std::mutex mu_;
  mutable std::unordered_map<int64_t, std::vector<uint8_t>> allowed_;
};

}  // namespace laya
