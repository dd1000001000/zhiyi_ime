// Laya (mmBERT) 分词器的 C++ 实现, 读取 Hugging Face tokenizer.json。
//
// 只实现该模型实际用到的组件:
//   added tokens 切分 (含 lstrip/rstrip) -> Replace(" " -> "▁") -> Metaspace(prepend=always, split)
//   -> BPE (byte_fallback, fuse_unk)
// 不加 BOS/EOS (Laya 全程 add_special_tokens=False)。正确性由 laya_cli --parity 对照 Python 验证。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace laya {

class BpeTokenizer {
 public:
  // 失败时抛 std::runtime_error。
  explicit BpeTokenizer(const std::string& tokenizer_json_path);

  std::vector<int64_t> encode(const std::string& text) const;

  int64_t token_id(const std::string& token) const;  // 不存在时返回 -1
  int64_t cls_id() const { return cls_id_; }
  int64_t sep_id() const { return sep_id_; }
  int64_t mask_id() const { return mask_id_; }
  int64_t pad_id() const { return pad_id_; }
  const std::string& mask_token() const { return mask_token_; }

 private:
  struct AddedToken {
    std::string content;
    int64_t id;
    bool lstrip, rstrip;
  };
  struct Merge {
    int rank;
    int64_t id;
  };

  void encode_segment(const std::string& segment, std::vector<int64_t>& out) const;
  void bpe_piece(const std::string& piece, std::vector<int64_t>& out) const;

  std::unordered_map<std::string, int64_t> vocab_;
  std::unordered_map<uint64_t, Merge> merges_;  // key = (left_id << 32) | right_id
  std::vector<AddedToken> added_;
  int64_t byte_ids_[256];
  int64_t unk_id_ = -1, cls_id_ = -1, sep_id_ = -1, mask_id_ = -1, pad_id_ = -1;
  std::string mask_token_ = "<mask>";

  mutable std::mutex cache_mu_;
  mutable std::unordered_map<std::string, std::vector<int64_t>> cache_;  // piece -> ids
};

}  // namespace laya
