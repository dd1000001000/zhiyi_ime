// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "model_sentences.h"

#include <algorithm>
#include <utility>

namespace laya {

namespace {

constexpr float kWritable = -1e3f;  // below this the slot's unit cannot spell the character

struct Row {
  double ln_p = 0.0;
  std::vector<int> ids;
};

}  // namespace

std::vector<ScoredSentence> best_sentences(const std::vector<std::vector<float>>& slots,
                                           const std::vector<std::string>& chars, size_t per_slot,
                                           size_t count) {
  if (slots.empty() || per_slot == 0 || count == 0) return {};
  std::vector<Row> rows(1);
  std::vector<std::pair<float, int>> top;
  for (const auto& slot : slots) {
    top.clear();
    const size_t n = (std::min)(slot.size(), chars.size());
    for (size_t k = 0; k < n; ++k) {
      if (slot[k] <= kWritable) continue;
      if (top.size() < per_slot) {
        top.emplace_back(slot[k], static_cast<int>(k));
        if (top.size() == per_slot) std::sort(top.begin(), top.end(), std::greater<>());
      } else if (slot[k] > top.back().first) {
        top.back() = {slot[k], static_cast<int>(k)};
        std::sort(top.begin(), top.end(), std::greater<>());
      }
    }
    if (top.empty()) return {};
    // Every row so far with every character kept here; the best `count` go on (independent
    // slots: the best rows of the next slot all extend the best rows of this one).
    std::vector<Row> next;
    next.reserve(rows.size() * top.size());
    for (const auto& row : rows) {
      for (const auto& [p, k] : top) {
        Row r{row.ln_p + p, row.ids};
        r.ids.push_back(k);
        next.push_back(std::move(r));
      }
    }
    const size_t keep = (std::min)(count, next.size());
    std::partial_sort(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(keep), next.end(),
                      [](const Row& a, const Row& b) { return a.ln_p > b.ln_p; });
    next.resize(keep);
    rows = std::move(next);
  }
  std::vector<ScoredSentence> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    ScoredSentence s;
    s.ln_p = row.ln_p;
    for (int k : row.ids) s.text += chars[static_cast<size_t>(k)];
    out.push_back(std::move(s));
  }
  return out;
}

bool sentence_ln_p(const std::vector<std::vector<float>>& slots, const std::vector<int>& ids, double& ln_p) {
  if (ids.size() != slots.size()) return false;
  ln_p = 0.0;
  for (size_t j = 0; j < ids.size(); ++j) {
    const int k = ids[j];
    const bool known = k >= 0 && static_cast<size_t>(k) < slots[j].size() && slots[j][static_cast<size_t>(k)] > kWritable;
    ln_p += known ? static_cast<double>(slots[j][static_cast<size_t>(k)]) : kUnwrittenLnP;
  }
  return true;
}

}  // namespace laya
