// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "next_words.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>

#include <json.hpp>  // third_party/nlohmann/json.hpp

#include "ngram.h"
#include "reranker.h"
#include "text_util.h"

namespace laya {
namespace {

// A Lite model's word: at least this many characters by its rows' counts (next_onnx.py min_chars).
constexpr int kMinWordChars = 2;

const std::string kMeta = "\xE2\x96\x81";  // "▁"

double log_add(double a, double b) {
  if (a == -std::numeric_limits<double>::infinity()) return b;
  if (b == -std::numeric_limits<double>::infinity()) return a;
  const double m = (std::max)(a, b);
  return m + std::log(std::exp(a - m) + std::exp(b - m));
}

size_t utf8_count(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

// log-softmax normalizer over the rows of one hypothesis' logits.
double log_sum(const float* x, const std::vector<int32_t>& rows) {
  double m = -std::numeric_limits<double>::infinity();
  for (int32_t r : rows) m = (std::max)(m, static_cast<double>(x[r]));
  double s = 0.0;
  for (int32_t r : rows) s += std::exp(static_cast<double>(x[r]) - m);
  return m + std::log(s);
}

std::string trim_spaces(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// A list of row texts; zhen-feather-1127 writes null for the tokens no mode can write (read as "").
std::vector<std::string> strings(const nlohmann::json& list) {
  std::vector<std::string> out;
  out.reserve(list.size());
  for (const auto& v : list) out.push_back(v.is_string() ? v.get<std::string>() : std::string());
  return out;
}

}  // namespace

NextWords::NextWords(const Reranker& model, const std::string& rows_json) : model_(model) {
  if (!model.has_next()) throw std::runtime_error("the model has no next-word decoder");
  std::ifstream f(std::filesystem::path(utf8_to_wide(rows_json)), std::ios::binary);
  if (!f) throw std::runtime_error("next_rows.json not found");
  const nlohmann::json j = nlohmann::json::parse(f);
  tokens_ = j.at("tokens").get<std::vector<int64_t>>();
  pieces_ = strings(j.at("pieces"));
  if (tokens_.empty() || pieces_.size() != tokens_.size()) throw std::runtime_error("next_rows.json: rows");
  const size_t R = tokens_.size();
  body_.resize(R);
  for (size_t r = 0; r < R; ++r) {
    const std::string& p = pieces_[r];
    body_[r] = p.size() > kMeta.size() && p.compare(0, kMeta.size(), kMeta) == 0 ? p.substr(kMeta.size()) : p;
    row_of_token_.emplace(tokens_[r], static_cast<int32_t>(r));
  }
  end_ = j.at("end").get<int32_t>();
  if (end_ < 0 || static_cast<size_t>(end_) >= R) throw std::runtime_error("next_rows.json: end");
  max_word_tokens_ = j.value("max_word_tokens", static_cast<size_t>(6));
  context_tokens_ = j.value("context_tokens", static_cast<size_t>(128));
  if (!j.contains("langs")) {  // a Lite model's rows: the zh and en modes
    lite_ = true;
    auto rows = [&](const char* key) {
      std::vector<uint8_t> mask(R, 0);
      for (int32_t r : j.at(key).get<std::vector<int32_t>>()) {
        if (r < 0 || static_cast<size_t>(r) >= R) throw std::runtime_error("next_rows.json: mask row");
        mask[static_cast<size_t>(r)] = 1;
      }
      return mask;
    };
    auto list = [&](const std::vector<uint8_t>& mask) {
      std::vector<int32_t> out;
      for (size_t r = 0; r < R; ++r)
        if (mask[r]) out.push_back(static_cast<int32_t>(r));
      return out;
    };
    auto per_row = [&](Lang& lg, const char* chars, const char* texts) {
      lg.chars = j.at(chars).get<std::vector<int32_t>>();
      lg.texts = strings(j.at(texts));
      if (lg.chars.size() != R || lg.texts.size() != R) throw std::runtime_error("next_rows.json: row texts");
    };
    Lang zh;
    zh.cjk = true;
    zh.allow = rows("zh");
    zh.allow[static_cast<size_t>(end_)] = 1;
    zh.allow_rows = list(zh.allow);
    per_row(zh, "chars_zh", "text_zh");
    Lang en;
    en.first = rows("en_start");
    en.cont = rows("en_cont");
    en.first[static_cast<size_t>(end_)] = 0;
    en.cont[static_cast<size_t>(end_)] = 1;
    en.allow.assign(R, 0);
    for (size_t r = 0; r < R; ++r) en.allow[r] = en.first[r] | en.cont[r];
    en.first_rows = list(en.first);
    en.cont_rows = list(en.cont);
    en.allow_rows = list(en.allow);
    per_row(en, "chars_en", "text_en");
    langs_.emplace("zh", std::move(zh));
    langs_.emplace("en", std::move(en));
    return;
  }
  const auto cjk = j.value("cjk", std::vector<std::string>{});
  const auto tags = j.value("lang_tags", nlohmann::json::object());
  for (const std::string& code : j.at("langs").get<std::vector<std::string>>()) {
    Lang lg;
    lg.cjk = std::find(cjk.begin(), cjk.end(), code) != cjk.end();
    if (tags.contains(code)) lg.tag = tags.at(code).get<std::vector<int64_t>>();
    auto load = [&](const char* kind, std::vector<uint8_t>& mask, std::vector<int32_t>& list) {
      mask.assign(R, 0);
      for (int32_t r : j.at(std::string(kind) + "_" + code).get<std::vector<int32_t>>()) {
        if (r < 0 || static_cast<size_t>(r) >= R) throw std::runtime_error("next_rows.json: mask row");
        mask[static_cast<size_t>(r)] = 1;
      }
      for (size_t r = 0; r < R; ++r)
        if (mask[r]) list.push_back(static_cast<int32_t>(r));
    };
    load("allow", lg.allow, lg.allow_rows);
    load("first", lg.first, lg.first_rows);
    load("cont", lg.cont, lg.cont_rows);
    langs_.emplace(code, std::move(lg));
  }
}

NextWords::~NextWords() = default;

std::vector<int64_t> NextWords::block(const std::string& context, const std::string& lang) const {
  const auto it = langs_.find(lang);
  const BpeTokenizer& tok = model_.tokenizer();
  std::string s = replace_all(context, tok.mask_token(), " ");
  if (it != langs_.end() && !it->second.cjk)  // the trailing white space (Python's rstrip(): line breaks too)
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' ||
                          s.back() == '\f' || s.back() == '\v'))
      s.pop_back();
  std::vector<int64_t> ids = s.empty() ? std::vector<int64_t>{} : tok.encode(s);
  if (ids.size() > context_tokens_) ids.erase(ids.begin(), ids.end() - static_cast<std::ptrdiff_t>(context_tokens_));
  std::vector<int64_t> out{tok.cls_id()};
  if (it != langs_.end()) out.insert(out.end(), it->second.tag.begin(), it->second.tag.end());
  out.insert(out.end(), ids.begin(), ids.end());
  out.push_back(tok.sep_id());
  return out;
}

std::string NextWords::text(const std::vector<int32_t>& rows, const Lang& lang, const std::string& code) const {
  std::string s;
  if (lite_) {  // the rows' texts (OnnxNext.text: Chinese without spaces, English trimmed, case kept)
    for (int32_t r : rows) s += lang.texts[static_cast<size_t>(r)];
    if (lang.cjk) s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return trim_spaces(s);
  }
  if (lang.cjk) {
    for (int32_t r : rows) s += body_[static_cast<size_t>(r)];
    return s;
  }
  // The tokenizer's decoder: "▁" as a space, byte pieces <0xHH> as their bytes.
  for (int32_t r : rows) {
    const std::string& p = pieces_[static_cast<size_t>(r)];
    if (p.size() == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' && p[5] == '>') {
      s.push_back(static_cast<char>(std::stoi(p.substr(3, 2), nullptr, 16)));
      continue;
    }
    s += replace_all(p, kMeta, " ");
  }
  s = trim_spaces(s);
  if (code == "en" || code == "vi")
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::vector<int32_t> NextWords::word_rows(const std::string& word, const Lang& lang, const std::string& code) const {
  const BpeTokenizer& tok = model_.tokenizer();
  const std::vector<int64_t> ids = lang.cjk ? tok.encode_no_prepend(word) : tok.encode(" " + word);
  if (ids.empty() || ids.size() > max_word_tokens_) return {};
  std::vector<int32_t> rows;
  for (int64_t id : ids) {
    const auto it = row_of_token_.find(id);
    if (it == row_of_token_.end()) return {};
    rows.push_back(it->second);
  }
  if (lang.cjk) {
    for (int32_t r : rows)
      if (!lang.allow[static_cast<size_t>(r)]) return {};
    return rows;
  }
  const bool vi = code == "vi" && !lite_;
  if (rows[0] == end_ || !(vi ? lang.allow : lang.first)[static_cast<size_t>(rows[0])]) return {};
  for (size_t i = 1; i < rows.size(); ++i)
    if (rows[i] == end_ || !(vi ? lang.allow : lang.cont)[static_cast<size_t>(rows[i])]) return {};
  return rows;
}

std::vector<NextWord> NextWords::predict(const std::string& context, const std::string& code, size_t k,
                                         size_t beam) const {
  const auto lit = langs_.find(code);
  if (lit == langs_.end() || k == 0 || beam == 0) return {};
  const Lang& lang = lit->second;
  const auto ctx = model_.next_context(block(context, code));
  if (!ctx) return {};
  const int64_t L = model_.next_layers(), H = model_.next_heads(), D = model_.next_head_dim();
  const bool vi = code == "vi" && !lite_;
  // The rows each step may write (bench OnnxNext.add): CJK any of the language's rows (the end
  // not first); spaced languages a word start first (Vietnamese any), then pieces or the end.
  std::vector<int32_t> step0, later;
  if (lang.cjk) {
    for (int32_t r : lang.allow_rows)
      if (r != end_) step0.push_back(r);
    later = lang.allow_rows;
  } else {
    for (int32_t r : vi ? lang.allow_rows : lang.first_rows)
      if (r != end_) step0.push_back(r);
    later = vi ? lang.allow_rows : lang.cont_rows;
    if (std::find(later.begin(), later.end(), end_) == later.end()) later.push_back(end_);
  }
  const std::vector<int32_t> only_end{end_};
  const size_t need = lang.cjk ? 2 : 1;
  // A Lite model's Chinese step 0: the softmax over the rows and the end, the end then not picked.
  const std::vector<int32_t>& norm0 = lite_ && lang.cjk ? lang.allow_rows : step0;
  auto complete = [&](const std::vector<int32_t>& rows, const std::string& w) {
    if (!lite_) return utf8_count(w) >= need;
    int chars = 0;
    for (int32_t r : rows) chars += lang.chars[static_cast<size_t>(r)];
    return chars >= kMinWordChars;
  };

  struct Hyp {
    double lp;
    std::vector<int32_t> rows;
    size_t parent;
  };
  std::vector<Hyp> beams{{0.0, {}, 0}};
  std::map<std::string, double> done;
  std::vector<float> dk(static_cast<size_t>(L * H * D), 0.0f), dv(dk.size(), 0.0f), logits, pk, pv;
  const size_t max_tok = max_word_tokens_;
  for (size_t step = 0; step <= max_tok && !beams.empty(); ++step) {
    const int64_t N = static_cast<int64_t>(beams.size());
    std::vector<int64_t> prev;
    for (const Hyp& h : beams) prev.push_back(h.rows.empty() ? 0 : h.rows.back());
    if (!model_.next_step(*ctx, prev, static_cast<int64_t>(step), dk, dv, &logits, &pk, &pv)) return {};
    const size_t R = logits.size() / static_cast<size_t>(N);
    const std::vector<int32_t>& allowed = step == 0 ? step0 : later;
    std::vector<Hyp> next;
    for (int64_t j = 0; j < N; ++j) {
      const float* x = logits.data() + static_cast<size_t>(j) * R;
      const double lse = log_sum(x, step == 0 ? norm0 : allowed);
      // The best `beam` of the allowed rows (at the last step only the end).
      const std::vector<int32_t>& pick_from = step == max_tok ? only_end : allowed;
      std::vector<std::pair<double, int32_t>> top;
      top.reserve(pick_from.size());
      for (int32_t r : pick_from) top.emplace_back(static_cast<double>(x[r]) - lse, r);
      const size_t keep = (std::min)(beam, top.size());
      std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(keep), top.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; });
      top.resize(keep);
      const Hyp& h = beams[static_cast<size_t>(j)];
      for (const auto& [v, r] : top) {
        if (r == end_) {
          const std::string w = text(h.rows, lang, code);
          if (complete(h.rows, w)) {
            auto it = done.find(w);
            const double cur = it == done.end() ? -std::numeric_limits<double>::infinity() : it->second;
            done[w] = log_add(cur, h.lp + v);
          }
        } else {
          Hyp n{h.lp + v, h.rows, static_cast<size_t>(j)};
          n.rows.push_back(r);
          next.push_back(std::move(n));
        }
      }
    }
    std::stable_sort(next.begin(), next.end(), [](const Hyp& a, const Hyp& b) { return a.lp > b.lp; });
    if (next.size() > beam) next.resize(beam);
    std::vector<double> best;
    for (const auto& [w, v] : done) best.push_back(v);
    std::sort(best.begin(), best.end(), std::greater<double>());
    if (best.size() >= k && (next.empty() || next.front().lp < best[k - 1])) next.clear();
    if (!next.empty()) {
      // present_* (L, N, H, step + 2, D): keep the hypotheses that go on, in their order.
      const int64_t P = static_cast<int64_t>(step) + 2;
      const size_t per = static_cast<size_t>(H * P * D);
      const int64_t M = static_cast<int64_t>(next.size());
      dk.assign(static_cast<size_t>(L * M) * per, 0.0f);
      dv.assign(dk.size(), 0.0f);
      for (int64_t l = 0; l < L; ++l) {
        for (int64_t m = 0; m < M; ++m) {
          const size_t src = static_cast<size_t>(l * N + static_cast<int64_t>(next[static_cast<size_t>(m)].parent)) * per;
          const size_t dst = static_cast<size_t>(l * M + m) * per;
          std::copy_n(pk.begin() + static_cast<std::ptrdiff_t>(src), per, dk.begin() + static_cast<std::ptrdiff_t>(dst));
          std::copy_n(pv.begin() + static_cast<std::ptrdiff_t>(src), per, dv.begin() + static_cast<std::ptrdiff_t>(dst));
        }
      }
    }
    beams = std::move(next);
  }
  std::vector<NextWord> out;
  for (const auto& [w, v] : done) out.push_back({w, v});
  std::stable_sort(out.begin(), out.end(), [](const NextWord& a, const NextWord& b) { return a.ln_p > b.ln_p; });
  if (out.size() > k) out.resize(k);
  return out;
}

std::vector<double> NextWords::score(const std::string& context, const std::string& code,
                                     const std::vector<std::string>& words) const {
  std::vector<double> out(words.size(), std::numeric_limits<double>::quiet_NaN());
  const auto lit = langs_.find(code);
  if (lit == langs_.end() || words.empty()) return out;
  const Lang& lang = lit->second;
  std::vector<size_t> which;
  std::vector<std::vector<int32_t>> paths;  // the word's rows, then the end
  for (size_t i = 0; i < words.size(); ++i) {
    std::vector<int32_t> rows = word_rows(words[i], lang, code);
    if (rows.empty()) continue;
    rows.push_back(end_);
    which.push_back(i);
    paths.push_back(std::move(rows));
  }
  if (paths.empty()) return out;
  const auto ctx = model_.next_context(block(context, code));
  if (!ctx) return out;
  const int64_t L = model_.next_layers(), H = model_.next_heads(), D = model_.next_head_dim();
  size_t longest = 0;
  for (const auto& p : paths) longest = (std::max)(longest, p.size());
  // Words sharing their first t rows read the same step t: one hypothesis per distinct prefix
  // (step 0 one for all), its keys and values gathered from its parent prefix's.
  std::vector<std::vector<int32_t>> prefixes{{}};  // the hypotheses of this step
  std::vector<size_t> prefix_of(paths.size(), 0);  // each word's hypothesis
  std::vector<float> dk(static_cast<size_t>(L * H * D), 0.0f), dv(dk.size(), 0.0f), logits, pk, pv;
  std::vector<double> total(paths.size(), 0.0);
  for (size_t t = 0; t < longest; ++t) {
    const int64_t N = static_cast<int64_t>(prefixes.size());
    std::vector<int64_t> prev;
    for (const auto& p : prefixes) prev.push_back(p.empty() ? 0 : p.back());
    if (!model_.next_step(*ctx, prev, static_cast<int64_t>(t), dk, dv, &logits, &pk, &pv)) return out;
    const size_t R = logits.size() / static_cast<size_t>(N);
    std::vector<double> lse(static_cast<size_t>(N), std::numeric_limits<double>::quiet_NaN());
    for (size_t n = 0; n < paths.size(); ++n) {
      if (t >= paths[n].size()) continue;
      const size_t h = prefix_of[n];
      const float* x = logits.data() + h * R;
      if (std::isnan(lse[h])) lse[h] = log_sum(x, lang.allow_rows);
      total[n] += static_cast<double>(x[paths[n][t]]) - lse[h];
    }
    // The next step's prefixes: each word still going on, its first t + 1 rows.
    std::map<std::vector<int32_t>, size_t> index;
    std::vector<std::vector<int32_t>> next;
    std::vector<size_t> parent;
    for (size_t n = 0; n < paths.size(); ++n) {
      if (t + 1 >= paths[n].size()) continue;
      std::vector<int32_t> p(paths[n].begin(), paths[n].begin() + static_cast<std::ptrdiff_t>(t + 1));
      auto [it, added] = index.emplace(p, next.size());
      if (added) {
        next.push_back(std::move(p));
        parent.push_back(prefix_of[n]);
      }
      prefix_of[n] = it->second;
    }
    if (next.empty()) break;
    const int64_t M = static_cast<int64_t>(next.size());
    const size_t per = static_cast<size_t>(H * (static_cast<int64_t>(t) + 2) * D);
    dk.assign(static_cast<size_t>(L * M) * per, 0.0f);
    dv.assign(dk.size(), 0.0f);
    for (int64_t l = 0; l < L; ++l) {
      for (int64_t m = 0; m < M; ++m) {
        const size_t src = static_cast<size_t>(l * N + static_cast<int64_t>(parent[static_cast<size_t>(m)])) * per;
        const size_t dst = static_cast<size_t>(l * M + m) * per;
        std::copy_n(pk.begin() + static_cast<std::ptrdiff_t>(src), per, dk.begin() + static_cast<std::ptrdiff_t>(dst));
        std::copy_n(pv.begin() + static_cast<std::ptrdiff_t>(src), per, dv.begin() + static_cast<std::ptrdiff_t>(dst));
      }
    }
    prefixes = std::move(next);
  }
  for (size_t n = 0; n < paths.size(); ++n) out[which[n]] = total[n];
  return out;
}

std::vector<NextWord> NextWords::predict_mixed(const std::string& context, const std::string& code, size_t k,
                                               size_t beam, const Ngram* ngram, double weight, size_t pool,
                                               size_t ngram_pool, double min_probability) const {
  const double min_lp = min_probability > 0 ? std::log(min_probability) : -std::numeric_limits<double>::infinity();
  auto cut = [&](std::vector<NextWord> words) {
    std::vector<NextWord> kept;
    for (NextWord& w : words)
      if (w.ln_p >= min_lp && kept.size() < k) kept.push_back(std::move(w));
    return kept;
  };
  if (!ngram || !ngram->is_open() || weight <= 0 || ngram_pool == 0 || code != "zh")
    return cut(predict(context, code, k, beam));
  std::vector<NextWord> gen = predict(context, code, (std::max)(pool, k), beam);
  std::unordered_map<std::string, double> dec;
  for (const NextWord& w : gen) dec[w.text] = w.ln_p;
  const auto ng_words = ngram->next_words(context, ngram_pool);
  std::unordered_map<std::string, double> ng;
  std::vector<std::string> to_score;
  for (const auto& [w, lp] : ng_words) {
    ng[w] = lp;
    if (!dec.count(w)) to_score.push_back(w);
  }
  const std::vector<double> sc = score(context, code, to_score);
  for (size_t i = 0; i < to_score.size(); ++i)
    if (!std::isnan(sc[i])) dec[to_score[i]] = sc[i];
  if (dec.empty() && ng.empty()) return {};
  double fd = std::numeric_limits<double>::infinity(), fn = std::numeric_limits<double>::infinity();
  for (const auto& [w, v] : dec) fd = (std::min)(fd, v);
  for (const auto& [w, v] : ng) fn = (std::min)(fn, v);
  fd = dec.empty() ? -30.0 : fd - 1.0;
  fn = ng.empty() ? -30.0 : fn - 1.0;
  std::vector<std::string> all;
  for (const auto& [w, v] : dec) all.push_back(w);
  for (const auto& [w, v] : ng)
    if (!dec.count(w)) all.push_back(w);
  auto value = [&](const std::string& w) {
    const auto a = dec.find(w);
    const auto b = ng.find(w);
    return (a == dec.end() ? fd : a->second) + weight * (b == ng.end() ? fn : b->second);
  };
  std::stable_sort(all.begin(), all.end(), [&](const std::string& a, const std::string& b) { return value(a) > value(b); });
  // The cut-off is the decoder's probability; the n-gram's own words stay (they are likely by the
  // n-gram, often below it by the decoder: cutting them lost more than it saved, dev/flashnext_ime).
  // The k best, less those cut (not filled up from further down: a page of fewer words).
  if (all.size() > k) all.resize(k);
  std::vector<NextWord> out;
  for (const std::string& w : all) {
    const auto a = dec.find(w);
    const double lp = a == dec.end() ? fd : a->second;
    if (lp >= min_lp || ng.count(w)) out.push_back({w, lp});
  }
  return out;
}

}  // namespace laya
