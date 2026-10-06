// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "reranker.h"

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include <json.hpp>  // cxx-ime vendors nlohmann/json as third_party/nlohmann/json.hpp
#include <onnxruntime_cxx_api.h>

#include "text_util.h"

namespace laya {

const char* const kInstructions =
    "The user is typing Chinese with a pinyin input method. "
    "Which candidate word is the most natural continuation of the text typed so far?";

const char* const kInstructionsEnglish =
    "The user is typing English with word completion. "
    "Which candidate word is the user most likely typing, given the text typed so far?";

const char* const kInstructionsGuess = "Which candidate word most likely comes next after the text so far?";

std::string decision_state(const std::string& context, const std::string& input, Task task) {
  return "已输入的上文: " + context + (task == Task::kEnglish ? "\n正在输入的英文: " : "\n正在输入的拼音: ") + input;
}

namespace {

std::string guess_state(const std::string& context) { return "已输入的上文: " + context; }

}  // namespace

Reranker::Reranker(const RerankerOptions& opt) {
  tok_ = std::make_unique<BpeTokenizer>(opt.model_dir + "/tokenizer.json");

  std::ifstream cf(utf8_to_wide(opt.model_dir + "/rl_agent_config.json"));
  if (cf) {
    auto cfg = nlohmann::json::parse(cf);
    max_len_ = cfg.value("max_len", max_len_);
    head_max_len_ = cfg.value("head_max_len", head_max_len_);
    // Our key, not Laya's: models fine-tuned on the guess prompt say so here.
    guess_prompt_ = cfg.value("zhiyi_prompt", std::string()) == "guess";
  }

  env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "laya");
  Ort::SessionOptions so;
  so.SetIntraOpNumThreads(opt.intra_threads);
  so.SetInterOpNumThreads(1);
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  std::wstring path = utf8_to_wide(opt.model_dir + "/" + opt.onnx_file);
  session_ = std::make_unique<Ort::Session>(*env_, path.c_str(), so);
  mem_ = std::make_unique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

  if (guess_prompt_) {
    head_ids_ = tok_->encode(std::string("choice question: ") +
                             replace_all(kInstructionsGuess, tok_->mask_token(), " "));
    head_ids_en_ = head_ids_;
  } else {
    std::string ins = replace_all(kInstructions, tok_->mask_token(), " ");
    head_ids_ = tok_->encode(std::string("choice question: ") + ins);
    head_ids_en_ = tok_->encode(std::string("choice question: ") +
                                replace_all(kInstructionsEnglish, tok_->mask_token(), " "));
  }
}

Reranker::~Reranker() = default;

Sequence Reranker::build(const std::string& context, const std::string& pinyin,
                         const std::vector<std::string>& candidates, Task task) const {
  const std::vector<int64_t>& head_ids = task == Task::kEnglish ? head_ids_en_ : head_ids_;
  const auto& tok = *tok_;
  const std::string& mask = tok.mask_token();
  // build_head: 每个选项 = [MASK] + encode(" " + "key: value")[:48], 这里 value 就是候选本身。
  std::vector<std::vector<int64_t>> opt_ids;
  size_t opt_total = 0;
  for (const auto& c : candidates) {
    std::string text = replace_all(" " + c + ": " + c, mask, " ");
    auto t = tok.encode(text);
    if (t.size() > 48) t.resize(48);
    std::vector<int64_t> o{tok.mask_id()};
    o.insert(o.end(), t.begin(), t.end());
    opt_total += o.size();
    opt_ids.push_back(std::move(o));
  }
  long opt_budget = static_cast<long>(head_max_len_) - static_cast<long>(opt_total);
  if (opt_budget < 16) {  // 选项太长时每个选项截断到同样长度
    const long n_opts = static_cast<long>(std::max<size_t>(1, opt_ids.size()));
    const size_t per = static_cast<size_t>(std::max<long>(4, (head_max_len_ - 16) / n_opts));
    opt_total = 0;
    for (auto& o : opt_ids) {
      if (o.size() > per) o.resize(per);
      opt_total += o.size();
    }
    opt_budget = static_cast<long>(head_max_len_) - static_cast<long>(opt_total);
  }
  size_t head_keep = static_cast<size_t>(std::max<long>(8, opt_budget));

  Sequence seq;
  seq.ids.push_back(tok.cls_id());
  seq.ids.insert(seq.ids.end(), head_ids.begin(), head_ids.begin() + std::min(head_keep, head_ids.size()));
  seq.ids.push_back(tok.sep_id());
  for (const auto& o : opt_ids) {
    seq.markers.push_back(static_cast<int64_t>(seq.ids.size()));
    seq.ids.insert(seq.ids.end(), o.begin(), o.end());
  }
  seq.ids.push_back(tok.sep_id());

  // build_sequence: 状态放在选项之后, 按剩余空间截断, 最后补 [SEP]。
  auto state_ids = tok.encode(replace_all(guess_prompt_ ? guess_state(context) : decision_state(context, pinyin, task),
                                          mask, " "));
  long room = std::max<long>(0, static_cast<long>(max_len_) - static_cast<long>(seq.ids.size()) - 1);
  if (static_cast<long>(state_ids.size()) > room) state_ids.resize(static_cast<size_t>(room));
  seq.ids.insert(seq.ids.end(), state_ids.begin(), state_ids.end());
  seq.ids.push_back(tok.sep_id());
  if (seq.ids.size() > static_cast<size_t>(max_len_)) seq.ids.resize(max_len_);
  seq.markers.erase(std::remove_if(seq.markers.begin(), seq.markers.end(),
                                   [&](int64_t m) { return m >= max_len_; }),
                    seq.markers.end());
  return seq;
}

std::vector<float> Reranker::score(const std::string& context, const std::string& pinyin,
                                   const std::vector<std::string>& candidates, Task task) const {
  if (candidates.empty()) return {};
  return score(build(context, pinyin, candidates, task));
}

std::vector<float> Reranker::score(const Sequence& seq) const {
  const int64_t L = static_cast<int64_t>(seq.ids.size());
  const int64_t K = static_cast<int64_t>(seq.markers.size());
  if (K == 0) return {};
  std::vector<int64_t> ids = seq.ids, att(L, 1), pos = seq.markers, qtype{0};  // 0 = choice
  std::unique_ptr<bool[]> mmask(new bool[K]);
  std::fill(mmask.get(), mmask.get() + K, true);

  const int64_t s_ids[2] = {1, L}, s_mark[2] = {1, K}, s_q[1] = {1};
  std::vector<Ort::Value> in;
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, ids.data(), ids.size(), s_ids, 2));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, att.data(), att.size(), s_ids, 2));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, pos.data(), pos.size(), s_mark, 2));
  in.push_back(Ort::Value::CreateTensor<bool>(*mem_, mmask.get(), K, s_mark, 2));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, qtype.data(), 1, s_q, 1));
  const char* in_names[] = {"input_ids", "attention_mask", "marker_pos", "marker_mask", "qtype"};
  const char* out_names[] = {"logits"};
  auto out = session_->Run(Ort::RunOptions{nullptr}, in_names, in.data(), in.size(), out_names, 1);

  const float* logits = out[0].GetTensorData<float>();
  std::vector<float> p(logits, logits + K);
  float mx = *std::max_element(p.begin(), p.end());
  float sum = 0;
  for (auto& v : p) sum += (v = std::exp(v - mx));
  for (auto& v : p) v /= sum;
  return p;
}

}  // namespace laya
