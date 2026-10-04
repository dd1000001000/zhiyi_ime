#include "tokenizer.h"

#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <json.hpp>  // cxx-ime vendors nlohmann/json as third_party/nlohmann/json.hpp

#include "text_util.h"

namespace laya {

namespace {
const std::string kMeta = "\xE2\x96\x81";  // "▁" (U+2581)

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

uint64_t pair_key(int64_t a, int64_t b) {
  return (static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b);
}
}  // namespace

BpeTokenizer::BpeTokenizer(const std::string& path) {
  std::ifstream f(utf8_to_wide(path), std::ios::binary);
  if (!f) throw std::runtime_error("cannot open tokenizer: " + path);
  nlohmann::json j = nlohmann::json::parse(f);
  const auto& model = j.at("model");
  if (model.at("type") != "BPE") throw std::runtime_error("only BPE tokenizers are supported");

  for (auto it = model.at("vocab").begin(); it != model.at("vocab").end(); ++it)
    vocab_.emplace(it.key(), it.value().get<int64_t>());

  int rank = 0;
  for (const auto& m : model.at("merges")) {
    std::string a, b;
    if (m.is_array()) {
      a = m[0].get<std::string>();
      b = m[1].get<std::string>();
    } else {  // 旧格式 "a b"
      std::string s = m.get<std::string>();
      size_t sp = s.find(' ');
      a = s.substr(0, sp);
      b = s.substr(sp + 1);
    }
    auto ia = vocab_.find(a), ib = vocab_.find(b), im = vocab_.find(a + b);
    if (ia != vocab_.end() && ib != vocab_.end() && im != vocab_.end())
      merges_.emplace(pair_key(ia->second, ib->second), Merge{rank, im->second});
    ++rank;
  }

  for (const auto& t : j.at("added_tokens")) {
    AddedToken a{t.at("content").get<std::string>(), t.at("id").get<int64_t>(),
                 t.value("lstrip", false), t.value("rstrip", false)};
    vocab_.emplace(a.content, a.id);
    added_.push_back(std::move(a));
  }

  for (int b = 0; b < 256; ++b) {
    char name[8];
    std::snprintf(name, sizeof(name), "<0x%02X>", b);
    byte_ids_[b] = token_id(name);
  }
  unk_id_ = token_id(model.value("unk_token", std::string("<unk>")));
  cls_id_ = token_id("<bos>");
  sep_id_ = token_id("<eos>");
  mask_id_ = token_id(mask_token_);
  pad_id_ = token_id("<pad>");
  if (cls_id_ < 0 || sep_id_ < 0 || mask_id_ < 0 || pad_id_ < 0)
    throw std::runtime_error("tokenizer is missing <bos>/<eos>/<mask>/<pad>");
}

int64_t BpeTokenizer::token_id(const std::string& token) const {
  auto it = vocab_.find(token);
  return it == vocab_.end() ? -1 : it->second;
}

std::vector<int64_t> BpeTokenizer::encode(const std::string& text) const {
  // 1. 先按 added tokens 切分 (最长匹配), 与 HF 一样它们不经过 normalizer。
  struct Seg {
    std::string text;
    int64_t added_id;  // -1 表示普通文本
  };
  std::vector<Seg> segs;
  std::string cur;
  size_t i = 0;
  while (i < text.size()) {
    const AddedToken* best = nullptr;
    for (const auto& a : added_)
      if (!a.content.empty() && text.compare(i, a.content.size(), a.content) == 0 &&
          (!best || a.content.size() > best->content.size()))
        best = &a;
    if (!best) {
      cur += text[i++];
      continue;
    }
    if (best->lstrip)
      while (!cur.empty() && is_space(cur.back())) cur.pop_back();
    if (!cur.empty()) segs.push_back({cur, -1});
    cur.clear();
    segs.push_back({best->content, best->id});
    i += best->content.size();
    if (best->rstrip)
      while (i < text.size() && is_space(text[i])) ++i;
  }
  if (!cur.empty()) segs.push_back({cur, -1});

  std::vector<int64_t> out;
  for (const auto& s : segs) {
    if (s.added_id >= 0)
      out.push_back(s.added_id);
    else
      encode_segment(s.text, out);
  }
  return out;
}

void BpeTokenizer::encode_segment(const std::string& segment, std::vector<int64_t>& out) const {
  // 2. Replace(" " -> "▁"), Metaspace: 不以 ▁ 开头则补一个, 再在每个 ▁ 前切开 (MergedWithNext)。
  std::string norm = replace_all(segment, " ", kMeta);
  if (norm.compare(0, kMeta.size(), kMeta) != 0) norm = kMeta + norm;
  size_t start = 0;
  for (size_t pos = kMeta.size(); pos <= norm.size(); ++pos) {
    bool boundary = pos == norm.size() || norm.compare(pos, kMeta.size(), kMeta) == 0;
    if (!boundary) continue;
    bpe_piece(norm.substr(start, pos - start), out);
    start = pos;
    if (pos < norm.size()) pos += kMeta.size() - 1;
  }
}

void BpeTokenizer::bpe_piece(const std::string& piece, std::vector<int64_t>& out) const {
  if (piece.empty()) return;
  {
    std::lock_guard<std::mutex> lk(cache_mu_);
    auto it = cache_.find(piece);
    if (it != cache_.end()) {
      out.insert(out.end(), it->second.begin(), it->second.end());
      return;
    }
  }
  // 3. 初始符号: 每个字符查词表, 查不到就拆成 <0xXX> 字节 token (byte_fallback)。
  std::vector<int64_t> sym;
  for (const auto& ch : utf8_chars(piece)) {
    int64_t id = token_id(ch);
    if (id >= 0) {
      sym.push_back(id);
      continue;
    }
    bool all_bytes = true;
    for (unsigned char c : ch) all_bytes = all_bytes && byte_ids_[c] >= 0;
    if (all_bytes) {
      for (unsigned char c : ch) sym.push_back(byte_ids_[c]);
    } else if (sym.empty() || sym.back() != unk_id_) {  // fuse_unk
      sym.push_back(unk_id_);
    }
  }
  // 4. 反复合并 rank 最小的相邻对 (rank 相同取最左), 与 HF BPE 的优先队列结果一致。
  while (sym.size() > 1) {
    int best_rank = std::numeric_limits<int>::max();
    size_t best_pos = 0;
    int64_t best_id = -1;
    for (size_t k = 0; k + 1 < sym.size(); ++k) {
      auto it = merges_.find(pair_key(sym[k], sym[k + 1]));
      if (it != merges_.end() && it->second.rank < best_rank) {
        best_rank = it->second.rank;
        best_pos = k;
        best_id = it->second.id;
      }
    }
    if (best_id < 0) break;
    sym[best_pos] = best_id;
    sym.erase(sym.begin() + static_cast<long>(best_pos) + 1);
  }
  out.insert(out.end(), sym.begin(), sym.end());
  std::lock_guard<std::mutex> lk(cache_mu_);
  if (cache_.size() > 50000) cache_.clear();
  cache_.emplace(piece, std::move(sym));
}

}  // namespace laya
