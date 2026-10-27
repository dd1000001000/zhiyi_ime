// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// The continuation head's own sentences (laya.model_sentences, laya_rerank.cc): the head writes
// every slot at once, so besides scoring the composer's dictionary words it can offer whole
// rows of its own, not split into dictionary words (long_test end to end, the last syllable
// typed in part: the first candidate 35.9% right before, 47.3% with them).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace laya {

struct ScoredSentence {
  std::string text;   // one character per slot
  double ln_p = 0.0;  // the head's, summed over the slots
};

// The ln P the head gives a character it does not write, or one the slot's unit cannot spell
// (as SpanScorer::score in laya_rerank.cc).
constexpr double kUnwrittenLnP = -20.0;

// The best `count` rows of `slots` (ln P per slot and character, -1e4 where the unit cannot spell
// it): each slot's `per_slot` most likely characters, rows by their summed ln P, best first.
// Empty when a slot has no character it may write.
std::vector<ScoredSentence> best_sentences(const std::vector<std::vector<float>>& slots,
                                           const std::vector<std::string>& chars, size_t per_slot,
                                           size_t count);

// ln P of a row given as character indices (-1 for one the head does not write), one per slot;
// false when the lengths differ.
bool sentence_ln_p(const std::vector<std::vector<float>>& slots, const std::vector<int>& ids, double& ln_p);

}  // namespace laya
