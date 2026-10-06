// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// Laya (mmBERT) 分词器的 C++ 实现, 读取 Hugging Face tokenizer.json。
//
// 只实现该模型实际用到的组件:
//   added tokens 切分 (含 lstrip/rstrip) -> Replace(" " -> "▁") -> Metaspace(prepend=always, split)
//   -> BPE (byte_fallback, fuse_unk)
// 不加 BOS/EOS (Laya 全程 add_special_tokens=False)。正确性由 laya_cli --parity 对照 Python 验证。
//
// 词表 (25.6 万词) 和 merges (58 万条) 不放在哈希表里, 而是一块排好序的紧凑镜像 (约 14 MB, 二分查找):
// 从 tokenizer.json 解析一次约 0.8 s、哈希表约 70 MB; 镜像可以写成 tokenizer.bin, 下次直接只读映射。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <cxxime/mapped_file.h>

namespace laya {

class BpeTokenizer {
 public:
  // 失败时抛 std::runtime_error。cache_bin 非空时: 文件存在就映射它 (不再读 JSON), 否则从 JSON
  // 建好镜像后写到那里 (写不成功就只留在内存里)。
  explicit BpeTokenizer(const std::string& tokenizer_json_path, const std::string& cache_bin = {});

  std::vector<int64_t> encode(const std::string& text) const;

  int64_t token_id(std::string_view token) const;  // 不存在时返回 -1
  int64_t cls_id() const { return hdr_->cls; }
  int64_t sep_id() const { return hdr_->sep; }
  int64_t mask_id() const { return hdr_->mask; }
  int64_t pad_id() const { return hdr_->pad; }
  const std::string& mask_token() const { return mask_token_; }
  bool mapped() const { return mapped_.is_open(); }  // the image came from cache_bin

 private:
  // The image: header, then 8-byte aligned sections at the offsets the header gives. All
  // offsets are from the start of the image; token bytes live in one blob.
  struct Header {
    char magic[8];
    uint32_t n_vocab, n_merges, n_added, blob_size;
    int64_t unk, cls, sep, mask, pad;
    int64_t byte_ids[256];
    uint32_t mask_off, mask_len;
    uint64_t vocab_off, vocab_id, merge_key, merge_val, added, blob;
  };
  struct Added {
    uint32_t off, len;
    int32_t id;
    uint8_t lstrip, rstrip, pad[2];
  };
  struct Merge {
    int32_t rank, id;
  };

  static std::string build_image(const std::string& json_path);
  void attach(const char* data, size_t size);
  std::string_view token_at(uint32_t index) const;
  const Merge* find_merge(uint64_t key) const;
  void encode_segment(const std::string& segment, std::vector<int64_t>& out) const;
  void bpe_piece(const std::string& piece, std::vector<int64_t>& out) const;

  std::string image_;           // owned image (built from JSON and not mapped)
  cxxime::MappedFile mapped_;   // or the cache file
  const Header* hdr_ = nullptr;
  const uint32_t* vocab_off_ = nullptr;  // n_vocab + 1 offsets into the blob, tokens sorted
  const int32_t* vocab_id_ = nullptr;
  const uint64_t* merge_key_ = nullptr;  // (left id << 32 | right id), sorted
  const Merge* merge_val_ = nullptr;
  const Added* added_ = nullptr;
  const char* blob_ = nullptr;
  std::string mask_token_ = "<mask>";

  mutable std::mutex cache_mu_;
  mutable std::unordered_map<std::string, std::vector<int64_t>> cache_;  // piece -> ids
};

}  // namespace laya
