// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "ngram.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "text_util.h"

namespace laya {

namespace {

constexpr double kScale = 1.0 / 2048.0;
constexpr double kLn10 = 2.302585092994046;

double value(int16_t q) { return static_cast<double>(q) * kScale; }

// The index of `w` in the sorted ids[begin, end), -1 for none.
int64_t find_sorted(const uint32_t* ids, uint32_t begin, uint32_t end, int64_t w) {
  const uint32_t* first = ids + begin;
  const uint32_t* last = ids + end;
  const uint32_t* it = std::lower_bound(first, last, static_cast<uint32_t>(w));
  return it != last && *it == static_cast<uint32_t>(w) ? static_cast<int64_t>(it - ids) : -1;
}

}  // namespace

bool Ngram::open(const std::string& path) {
  words_ = nullptr;
  file_.close();
  if (!file_.open(path)) return false;
  const char* base = file_.data();
  const size_t size = file_.size();
  if (size < 24 || std::memcmp(base, "ZYNGRAM1", 8) != 0) {
    file_.close();
    return false;
  }
  uint32_t head[4];
  std::memcpy(head, base + 8, sizeof(head));
  n1_ = head[0];
  n2_ = head[1];
  n3_ = head[2];
  size_t pos = 24;
  bool ok = n1_ > 0;
  // Each array starts 8-byte aligned; false when the file is shorter than its counts say.
  auto take = [&](size_t bytes) -> const char* {
    pos = (pos + 7) & ~size_t{7};
    if (!ok || bytes > size || pos > size - bytes) {
      ok = false;
      return nullptr;
    }
    const char* p = base + pos;
    pos += bytes;
    return p;
  };
  word_off_ = reinterpret_cast<const uint32_t*>(take((size_t{n1_} + 1) * 4));
  const char* words = ok ? take(word_off_[n1_]) : nullptr;
  p1_ = reinterpret_cast<const int16_t*>(take(size_t{n1_} * 2));
  b1_ = reinterpret_cast<const int16_t*>(take(size_t{n1_} * 2));
  bi_start_ = reinterpret_cast<const uint32_t*>(take((size_t{n1_} + 1) * 4));
  bi_w_ = reinterpret_cast<const uint32_t*>(take(size_t{n2_} * 4));
  bi_p_ = reinterpret_cast<const int16_t*>(take(size_t{n2_} * 2));
  bi_b_ = reinterpret_cast<const int16_t*>(take(size_t{n2_} * 2));
  tri_start_ = reinterpret_cast<const uint32_t*>(take((size_t{n2_} + 1) * 4));
  tri_w_ = reinterpret_cast<const uint32_t*>(take(size_t{n3_} * 4));
  tri_p_ = reinterpret_cast<const int16_t*>(take(size_t{n3_} * 2));
  if (!ok || bi_start_[n1_] != n2_ || tri_start_[n2_] != n3_) {
    file_.close();
    return false;
  }
  words_ = words;
  unk_ = word("<unk>");
  if (unk_ < 0) unk_ = 0;  // as bench ngram_lm.py: word 0 stands for a character of no word
  return true;
}

int64_t Ngram::word(std::string_view text) const {
  if (!words_) return -1;
  // The words are sorted bytewise (std::string_view compares as unsigned char).
  uint32_t lo = 0, hi = n1_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const std::string_view w(words_ + word_off_[mid], word_off_[mid + 1] - word_off_[mid]);
    const int c = w.compare(text);
    if (c == 0) return mid;
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

int64_t Ngram::bigram(int64_t v, int64_t w) const {
  return find_sorted(bi_w_, bi_start_[v], bi_start_[v + 1], w);
}

double Ngram::log10_p(int64_t v, int64_t w) const {
  if (v < 0) return value(p1_[w]);
  const int64_t j = bigram(v, w);
  return j >= 0 ? value(bi_p_[j]) : value(b1_[v]) + value(p1_[w]);
}

double Ngram::log10_p(int64_t u, int64_t v, int64_t w) const {
  if (v < 0) return value(p1_[w]);
  if (u < 0) return log10_p(v, w);
  const int64_t ctx = bigram(u, v);
  if (ctx < 0) return log10_p(v, w);  // a context with no bigram of its own backs off by nothing
  const int64_t k = find_sorted(tri_w_, tri_start_[ctx], tri_start_[ctx + 1], w);
  if (k >= 0) return value(tri_p_[k]);
  return value(bi_b_[ctx]) + log10_p(v, w);
}

std::vector<Ngram::State> Ngram::run(const std::vector<std::string>& chars, size_t start,
                                     std::vector<State> states) const {
  const size_t n = chars.size();
  std::vector<std::vector<State>> at(n + 1);
  at[start] = std::move(states);
  std::vector<std::pair<size_t, int64_t>> edges;
  std::string text;
  for (size_t i = start; i < n; ++i) {
    std::vector<State>& here = at[i];
    if (here.empty()) continue;
    // The best kBeam states, ties in the order they were reached (as Python's stable sort).
    std::stable_sort(here.begin(), here.end(), [](const State& a, const State& b) { return a.score > b.score; });
    if (here.size() > kBeam) here.resize(kBeam);
    edges.clear();
    bool single = false;
    text.clear();
    for (size_t len = 1; len <= kMaxWordChars && i + len <= n; ++len) {
      text += chars[i + len - 1];
      const int64_t w = word(text);
      if (w < 0) continue;
      edges.emplace_back(i + len, w);
      single = single || len == 1;
    }
    if (!single) edges.emplace_back(i + 1, unk_);
    for (const State& s : here) {
      for (const auto& [j, w] : edges) {
        const double t = s.score + log10_p(s.u, s.v, w);
        std::vector<State>& there = at[j];
        auto it = std::find_if(there.begin(), there.end(),
                               [&](const State& o) { return o.u == s.v && o.v == w; });
        if (it == there.end()) {
          there.push_back({s.v, w, t});
        } else if (t > it->score) {
          it->score = t;
        }
      }
    }
  }
  return std::move(at[n]);
}

std::vector<double> Ngram::score(const std::string& context, const std::vector<std::string>& texts) const {
  std::vector<double> out(texts.size(), -std::numeric_limits<double>::infinity());
  if (!words_) return out;
  // The context's last line, its last kContextChars characters.
  const size_t nl = context.find_last_of("\r\n");
  std::vector<std::string> ctx = utf8_chars(nl == std::string::npos ? context : context.substr(nl + 1));
  if (ctx.size() > kContextChars) ctx.erase(ctx.begin(), ctx.end() - static_cast<std::ptrdiff_t>(kContextChars));
  std::vector<State> top{{-1, -1, 0.0}};
  if (!ctx.empty()) top = run(ctx, 0, std::move(top));
  std::stable_sort(top.begin(), top.end(), [](const State& a, const State& b) { return a.score > b.score; });
  if (top.size() > kBeam) top.resize(kBeam);
  if (top.empty()) return out;
  const double base = top.front().score;
  for (size_t k = 0; k < texts.size(); ++k) {
    std::vector<std::string> chars = ctx;
    for (std::string& c : utf8_chars(texts[k])) chars.push_back(std::move(c));
    const std::vector<State> end = run(chars, ctx.size(), top);
    double best = -std::numeric_limits<double>::infinity();
    for (const State& s : end) best = (std::max)(best, s.score);
    out[k] = kLn10 * (best - base);
  }
  return out;
}

}  // namespace laya
