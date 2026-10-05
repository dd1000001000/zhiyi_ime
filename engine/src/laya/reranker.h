// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// Laya 候选重排: 构造与训练时完全一致的输入序列, 用 ONNX Runtime (CPU) 推理。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tokenizer.h"

namespace Ort {
struct Env;
struct Session;
struct MemoryInfo;
}  // namespace Ort

namespace laya {

// 必须与训练时的 INSTRUCTIONS / INSTRUCTIONS_EN / decision_state 格式完全一致 (模型就是按这个格式训练的)。
enum class Task { kPinyin, kEnglish };  // 拼音同音词 / 英文单词补全
extern const char* const kInstructions;
extern const char* const kInstructionsEnglish;
// 猜词提示词 (rl_agent_config.json 里 "zhiyi_prompt": "guess" 的模型): 不给拼音 / 已打字母,
// 只凭上文从候选里猜下一个词, 中英文共用。
extern const char* const kInstructionsGuess;
// kPinyin: input = "jin'tian"; kEnglish: input = 已打的字母 ("comp")
std::string decision_state(const std::string& context, const std::string& input, Task task = Task::kPinyin);

struct Sequence {
  std::vector<int64_t> ids;
  std::vector<int64_t> markers;  // 每个候选的 [MASK] 位置
};

struct RerankerOptions {
  std::string model_dir;                  // 含 tokenizer.json, rl_agent_config.json
  std::string onnx_file = "laya.int8g.onnx";
  int intra_threads = 4;
};

class Reranker {
 public:
  explicit Reranker(const RerankerOptions& opt);  // 失败抛异常
  ~Reranker();

  // laya.common.build_sequence 的移植 (choice 题, criteria = {候选: 候选})。
  Sequence build(const std::string& context, const std::string& pinyin,
                 const std::vector<std::string>& candidates, Task task = Task::kPinyin) const;

  // 返回每个候选的概率 (softmax(logits), 温度为 1)。线程安全。
  std::vector<float> score(const std::string& context, const std::string& pinyin,
                           const std::vector<std::string>& candidates, Task task = Task::kPinyin) const;
  std::vector<float> score(const Sequence& seq) const;

  const BpeTokenizer& tokenizer() const { return *tok_; }
  bool guess_prompt() const { return guess_prompt_; }

 private:
  std::unique_ptr<BpeTokenizer> tok_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::unique_ptr<Ort::MemoryInfo> mem_;
  std::vector<int64_t> head_ids_;     // "choice question: <instructions>" 只编码一次
  std::vector<int64_t> head_ids_en_;  // 英文任务的 instructions
  int max_len_ = 1024;
  int head_max_len_ = 256;
  bool guess_prompt_ = false;
};

}  // namespace laya
