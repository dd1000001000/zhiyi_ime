// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// Laya 候选重排: 构造与训练时完全一致的输入序列, 用 ONNX Runtime 推理 (CPU, 或 DirectML 显卡)。
//
// Two generations of model file share this class. The deployed choice model reads
// input_ids / attention_mask / marker_pos / marker_mask / qtype and answers "logits". The
// unified model (bench export_unified.py) also reads unit_ids and slot_pos: the typed input as
// slots ([MASK] + a unit vector per syllable or letter) after the choice sequence, and a second
// output, "char_logits", the continuation head's score of every character at every slot. The
// graph's input names say which one was loaded (has_slots()). A decoder model (bench
// export_unified_dec.py) gives "memory" and "slot_vec" instead, what its decoder graph (a second
// file, unified_vocab.json names it) reads to write one character at a time (encode / decode).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "slots.h"
#include "tokenizer.h"

namespace Ort {
struct Env;
struct Session;
struct MemoryInfo;
}  // namespace Ort

namespace laya {

enum class Task { kPinyin, kEnglish };  // 拼音同音词 / 英文单词补全
// 猜词提示词, 必须与训练时完全一致: 不给拼音 / 已打字母, 只凭上文从候选里猜下一个词, 中英文共用。
extern const char* const kInstructionsGuess;

struct Sequence {
  std::vector<int64_t> ids;
  std::vector<int64_t> markers;  // 每个候选的 [MASK] 位置
  std::vector<int64_t> units;    // the unit vector row of each token (0 = none); empty = no slots
  std::vector<int64_t> slots;    // the slot positions (continuation: where the head is read)
};

// A decoder model's reading of one continuation sequence (encode): what its decoder reads at
// every character it writes (decode). About length x width floats (160 x 384: 245 KB).
struct Encoded {
  int64_t length = 0;  // tokens of the sequence (the card's fixed length on DirectML)
  int64_t width = 0;
  int64_t slots = 0;
  std::vector<float> memory;      // (length, width)
  std::vector<int64_t> mask;      // (length): 1 = a token
  std::vector<float> slot_vec;    // (slots, width)
  std::vector<int64_t> unit_ids;  // (slots): the unit of each slot
  size_t bytes() const {
    return memory.size() * sizeof(float) + mask.size() * sizeof(int64_t) + slot_vec.size() * sizeof(float) +
           unit_ids.size() * sizeof(int64_t);
  }
};
inline size_t cache_bytes(const Encoded& e) { return e.bytes(); }

// The KV cache of one reading (decode with the step graphs): the memory's cross-attention keys
// and values, computed on the first call, and the self-attention keys and values of every path
// the last call wrote, by its characters. The next call reads them instead of every earlier
// character again. One thread at a time.
struct DecoderCache {
  std::vector<int64_t> cross_shape;  // (layers, heads, length, head_dim); empty before the first call
  std::vector<float> cross_k, cross_v;
  int64_t positions = 0;  // slots each path below holds
  // a path's characters -> its keys, values (layers, heads, positions, head_dim)
  std::unordered_map<std::string, std::pair<std::vector<float>, std::vector<float>>> paths;
};

struct RerankerOptions {
  std::string model_dir;                  // 含 tokenizer.json, rl_agent_config.json
  std::string onnx_file = "laya.int8g.onnx";
  int intra_threads = 4;
  // Directory for the prepacked copy of the model (see Reranker); empty = load the model as is.
  std::string cache_dir;
  // >= 0: run on this graphics card (DirectML device_id, the DXGI adapter index) instead of the
  // CPU. The card gets one fixed input shape (kGpuSequenceLength tokens, kGpuMarkers
  // candidates): DirectML compiles the whole graph for a fixed shape, and runs it about ten
  // times slower when shapes vary.
  int gpu_device = -1;
};

constexpr int kGpuSequenceLength = 256;
constexpr int kGpuMarkers = 32;
constexpr int kGpuSlots = 24;

// The model with every Reshape's allowzero cleared (DirectML fails on allowzero = 1; no shape in
// this model has a zero, so the result is the same). Patched in the serialized model: the
// attribute's int field is one byte either way. Returns how many were cleared.
size_t clear_reshape_allowzero(std::vector<char>& model);

class Reranker {
 public:
  explicit Reranker(const RerankerOptions& opt);  // 失败抛异常
  ~Reranker();

  // laya.common.build_sequence 的移植 (choice 题, criteria = {候选: 候选})。pinyin 和 task 不进
  // 提示词 (猜词提示词只有上文)。With a unified model, task == kEnglish adds `pinyin`'s letters
  // as slots; kPinyin adds none (the overload below takes the pinyin units).
  Sequence build(const std::string& context, const std::string& pinyin,
                 const std::vector<std::string>& candidates, Task task = Task::kPinyin) const;
  // The choice sequence followed by the typed input as slots (ignored by a model without them).
  Sequence build(const std::string& context, const std::vector<std::string>& candidates,
                 const std::vector<Unit>& units) const;

  // 返回每个候选的概率 (softmax(logits), 温度为 1)。线程安全。
  std::vector<float> score(const std::string& context, const std::string& pinyin,
                           const std::vector<std::string>& candidates, Task task = Task::kPinyin) const;
  std::vector<float> score(const std::string& context, const std::vector<std::string>& candidates,
                           const std::vector<Unit>& units) const;
  std::vector<float> score(const Sequence& seq) const;

  // The continuation head (unified model only): for each slot, ln P over the vocabulary's
  // characters (-1e4 for the characters the slot's unit cannot spell), the sequence being
  // [CLS] 已输入的上文: context [SEP] [MASK] x slots [SEP] (p2c_common.encode). Empty when the
  // model has no continuation head, there are no units, or more than the model's maximum.
  std::vector<std::vector<float>> continuation(const std::string& context,
                                               const std::vector<Unit>& units) const;

  // A decoder model (has_decoder): the sequence of continuation() read once; nullptr when the
  // model has no decoder, there are no units, more than its maximum, or the model fails.
  std::shared_ptr<const Encoded> encode(const std::string& context, const std::vector<Unit>& units) const;
  // ... then, for each prefix (the characters chosen at the slots before `slot`, as indices into
  // vocab()->chars(), -1 for one the model does not write), ln P over the characters at `slot`
  // (-1e4 for those its unit cannot spell). One decoder call for all the prefixes. Empty when
  // the model fails. With a cache (has_decoder_cache) and the slots written in order from 0,
  // only the new character is computed; otherwise every earlier one is read again.
  std::vector<std::vector<float>> decode(const Encoded& encoded, const std::vector<std::vector<int>>& prefixes,
                                         size_t slot, DecoderCache* cache = nullptr) const;
  bool has_decoder() const { return decoder_ != nullptr; }
  bool has_decoder_cache() const { return decoder_memory_ && decoder_step_; }

  bool has_slots() const { return vocab_ != nullptr; }  // a unified model
  const UnifiedVocab* vocab() const { return vocab_.get(); }
  const BpeTokenizer& tokenizer() const { return *tok_; }
  const std::string& load_note() const { return load_note_; }  // how the model was loaded (log)
  bool on_gpu() const { return gpu_; }

 private:
  void append_slots(Sequence& seq, const std::vector<Unit>& units) const;
  // p2c_common.encode: [CLS] 已输入的上文: context [SEP] [MASK] x slots [SEP].
  Sequence continuation_sequence(const std::string& context, const std::vector<Unit>& units) const;
  bool run(Sequence seq, std::vector<float>* logits, std::vector<float>* char_logits,
           size_t* chars, Encoded* encoded) const;
  std::vector<std::vector<float>> decode_cached(const Encoded& encoded, const std::vector<std::vector<int>>& prefixes,
                                                size_t slot, DecoderCache& cache) const;

  std::unique_ptr<BpeTokenizer> tok_;
  std::unique_ptr<UnifiedVocab> vocab_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::unique_ptr<Ort::Session> decoder_;  // a decoder model's second graph (the CPU)
  std::unique_ptr<Ort::Session> decoder_memory_;  // ... and its KV cache graphs, when there
  std::unique_ptr<Ort::Session> decoder_step_;
  std::string load_note_;
  std::unique_ptr<Ort::MemoryInfo> mem_;
  std::vector<int64_t> head_ids_;  // "choice question: <instructions>" 只编码一次
  std::vector<int64_t> context_prefix_ids_;  // the continuation sequence's "已输入的上文: "
  int max_len_ = 1024;
  int head_max_len_ = 256;
  bool gpu_ = false;
  // DirectML runs one call of a session at a time (the key path and the background
  // continuation share the model); the CPU takes them together.
  mutable std::mutex gpu_run_mutex_;
  bool slots_input_ = false;  // the graph reads unit_ids / slot_pos
  bool memory_output_ = false;  // the graph gives memory / slot_vec (a decoder model)
};

}  // namespace laya
