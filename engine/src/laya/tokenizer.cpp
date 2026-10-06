// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "tokenizer.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <json.hpp>  // cxx-ime vendors nlohmann/json as third_party/nlohmann/json.hpp

#include "text_util.h"

namespace laya {

namespace {
const std::string kMeta = "\xE2\x96\x81";  // "▁" (U+2581)
constexpr char kMagic[8] = {'Z', 'Y', 'T', 'O', 'K', '0', '0', '1'};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

uint64_t pair_key(int64_t a, int64_t b) {
  return (static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b);
}

// Appends `bytes` to the image at an 8-byte aligned offset, returning that offset.
uint64_t append_section(std::string& image, const void* bytes, size_t size) {
  image.resize((image.size() + 7) & ~size_t{7});
  const uint64_t off = image.size();
  image.append(static_cast<const char*>(bytes), size);
  return off;
}

template <typename T>
uint64_t append_section(std::string& image, const std::vector<T>& v) {
  return append_section(image, v.data(), v.size() * sizeof(T));
}

bool write_file_atomically(const std::string& path, const std::string& bytes) {
  std::error_code ec;
  const std::filesystem::path target(utf8_to_wide(path));
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::filesystem::path tmp = target.wstring() + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
  }
  if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}
}  // namespace

// Parses tokenizer.json into the sorted, flat image described in tokenizer.h.
std::string BpeTokenizer::build_image(const std::string& path) {
  std::ifstream f(utf8_to_wide(path), std::ios::binary);
  if (!f) throw std::runtime_error("cannot open tokenizer: " + path);
  nlohmann::json j = nlohmann::json::parse(f);
  const auto& model = j.at("model");
  if (model.at("type") != "BPE") throw std::runtime_error("only BPE tokenizers are supported");

  std::unordered_map<std::string, int64_t> vocab;
  for (auto it = model.at("vocab").begin(); it != model.at("vocab").end(); ++it)
    vocab.emplace(it.key(), it.value().get<int64_t>());

  std::vector<std::pair<uint64_t, Merge>> merges;
  int32_t rank = 0;
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
    auto ia = vocab.find(a), ib = vocab.find(b), im = vocab.find(a + b);
    if (ia != vocab.end() && ib != vocab.end() && im != vocab.end())
      merges.emplace_back(pair_key(ia->second, ib->second), Merge{rank, static_cast<int32_t>(im->second)});
    ++rank;
  }
  std::sort(merges.begin(), merges.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });

  struct AddedSrc {
    std::string content;
    int64_t id;
    bool lstrip, rstrip;
  };
  std::vector<AddedSrc> added;
  for (const auto& t : j.at("added_tokens")) {
    AddedSrc a{t.at("content").get<std::string>(), t.at("id").get<int64_t>(),
               t.value("lstrip", false), t.value("rstrip", false)};
    vocab.emplace(a.content, a.id);  // like the vocab entries: the first definition wins
    added.push_back(std::move(a));
  }

  std::vector<std::pair<std::string, int64_t>> sorted(vocab.begin(), vocab.end());
  std::sort(sorted.begin(), sorted.end());
  auto lookup = [&](const std::string& token) -> int64_t {
    auto it = vocab.find(token);
    return it == vocab.end() ? -1 : it->second;
  };

  Header h{};
  std::memcpy(h.magic, kMagic, sizeof(kMagic));
  h.n_vocab = static_cast<uint32_t>(sorted.size());
  h.n_merges = static_cast<uint32_t>(merges.size());
  h.n_added = static_cast<uint32_t>(added.size());
  for (int b = 0; b < 256; ++b) {
    char name[8];
    std::snprintf(name, sizeof(name), "<0x%02X>", b);
    h.byte_ids[b] = lookup(name);
  }
  h.unk = lookup(model.value("unk_token", std::string("<unk>")));
  h.cls = lookup("<bos>");
  h.sep = lookup("<eos>");
  h.mask = lookup("<mask>");
  h.pad = lookup("<pad>");
  if (h.cls < 0 || h.sep < 0 || h.mask < 0 || h.pad < 0)
    throw std::runtime_error("tokenizer is missing <bos>/<eos>/<mask>/<pad>");

  std::string blob;
  std::vector<uint32_t> vocab_off;
  std::vector<int32_t> vocab_id;
  vocab_off.reserve(sorted.size() + 1);
  vocab_id.reserve(sorted.size());
  for (const auto& [token, id] : sorted) {
    vocab_off.push_back(static_cast<uint32_t>(blob.size()));
    vocab_id.push_back(static_cast<int32_t>(id));
    blob += token;
  }
  vocab_off.push_back(static_cast<uint32_t>(blob.size()));
  std::vector<uint64_t> merge_key;
  std::vector<Merge> merge_val;
  merge_key.reserve(merges.size());
  merge_val.reserve(merges.size());
  for (const auto& [key, val] : merges) {
    merge_key.push_back(key);
    merge_val.push_back(val);
  }
  std::vector<Added> added_rec;
  for (const auto& a : added) {
    added_rec.push_back(Added{static_cast<uint32_t>(blob.size()), static_cast<uint32_t>(a.content.size()),
                              static_cast<int32_t>(a.id), a.lstrip, a.rstrip, {0, 0}});
    blob += a.content;
  }
  h.mask_off = static_cast<uint32_t>(blob.size());
  h.mask_len = 6;
  blob += "<mask>";
  h.blob_size = static_cast<uint32_t>(blob.size());

  std::string image(sizeof(Header), '\0');
  h.vocab_off = append_section(image, vocab_off);
  h.vocab_id = append_section(image, vocab_id);
  h.merge_key = append_section(image, merge_key);
  h.merge_val = append_section(image, merge_val);
  h.added = append_section(image, added_rec);
  h.blob = append_section(image, blob.data(), blob.size());
  std::memcpy(image.data(), &h, sizeof(h));
  return image;
}

void BpeTokenizer::attach(const char* data, size_t size) {
  auto bad = [](const char* what) { throw std::runtime_error(std::string("tokenizer image: ") + what); };
  if (size < sizeof(Header)) bad("too small");
  const Header* h = reinterpret_cast<const Header*>(data);
  if (std::memcmp(h->magic, kMagic, sizeof(kMagic)) != 0) bad("bad magic");
  auto section = [&](uint64_t off, uint64_t bytes) {
    if (off % 8 != 0 || off > size || bytes > size - off) bad("section out of range");
    return data + off;
  };
  vocab_off_ = reinterpret_cast<const uint32_t*>(section(h->vocab_off, (h->n_vocab + 1ull) * sizeof(uint32_t)));
  vocab_id_ = reinterpret_cast<const int32_t*>(section(h->vocab_id, h->n_vocab * sizeof(int32_t)));
  merge_key_ = reinterpret_cast<const uint64_t*>(section(h->merge_key, h->n_merges * sizeof(uint64_t)));
  merge_val_ = reinterpret_cast<const Merge*>(section(h->merge_val, h->n_merges * sizeof(Merge)));
  added_ = reinterpret_cast<const Added*>(section(h->added, h->n_added * sizeof(Added)));
  blob_ = section(h->blob, h->blob_size);
  if (vocab_off_[h->n_vocab] > h->blob_size || h->mask_off + h->mask_len > h->blob_size) bad("blob out of range");
  for (uint32_t i = 0; i < h->n_added; ++i)
    if (added_[i].off + static_cast<uint64_t>(added_[i].len) > h->blob_size) bad("added token out of range");
  hdr_ = h;
  mask_token_.assign(blob_ + h->mask_off, h->mask_len);
}

BpeTokenizer::BpeTokenizer(const std::string& json_path, const std::string& cache_bin) {
  if (!cache_bin.empty() && mapped_.open(cache_bin)) {
    try {
      attach(mapped_.data(), mapped_.size());
      return;
    } catch (const std::exception&) {  // a truncated or foreign file: rebuild it from the JSON
      hdr_ = nullptr;
      mapped_.close();
    }
  }
  image_ = build_image(json_path);
  if (!cache_bin.empty() && write_file_atomically(cache_bin, image_) && mapped_.open(cache_bin)) {
    attach(mapped_.data(), mapped_.size());
    std::string().swap(image_);
    return;
  }
  attach(image_.data(), image_.size());
}

std::string_view BpeTokenizer::token_at(uint32_t index) const {
  return std::string_view(blob_ + vocab_off_[index], vocab_off_[index + 1] - vocab_off_[index]);
}

int64_t BpeTokenizer::token_id(std::string_view token) const {
  uint32_t lo = 0, hi = hdr_->n_vocab;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const int c = token_at(mid).compare(token);
    if (c == 0) return vocab_id_[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return -1;
}

const BpeTokenizer::Merge* BpeTokenizer::find_merge(uint64_t key) const {
  const uint64_t* end = merge_key_ + hdr_->n_merges;
  const uint64_t* it = std::lower_bound(merge_key_, end, key);
  return it != end && *it == key ? merge_val_ + (it - merge_key_) : nullptr;
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
    const Added* best = nullptr;
    for (uint32_t k = 0; k < hdr_->n_added; ++k) {
      const Added& a = added_[k];
      if (a.len != 0 && text.compare(i, a.len, blob_ + a.off, a.len) == 0 && (!best || a.len > best->len))
        best = &a;
    }
    if (!best) {
      cur += text[i++];
      continue;
    }
    if (best->lstrip)
      while (!cur.empty() && is_space(cur.back())) cur.pop_back();
    if (!cur.empty()) segs.push_back({cur, -1});
    cur.clear();
    segs.push_back({std::string(blob_ + best->off, best->len), best->id});
    i += best->len;
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
    for (unsigned char c : ch) all_bytes = all_bytes && hdr_->byte_ids[c] >= 0;
    if (all_bytes) {
      for (unsigned char c : ch) sym.push_back(hdr_->byte_ids[c]);
    } else if (sym.empty() || sym.back() != hdr_->unk) {  // fuse_unk
      sym.push_back(hdr_->unk);
    }
  }
  // 4. 反复合并 rank 最小的相邻对 (rank 相同取最左), 与 HF BPE 的优先队列结果一致。
  while (sym.size() > 1) {
    int32_t best_rank = std::numeric_limits<int32_t>::max();
    size_t best_pos = 0;
    int64_t best_id = -1;
    for (size_t k = 0; k + 1 < sym.size(); ++k) {
      const Merge* m = find_merge(pair_key(sym[k], sym[k + 1]));
      if (m && m->rank < best_rank) {
        best_rank = m->rank;
        best_pos = k;
        best_id = m->id;
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
