// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "reranker.h"

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

// ---- Prepacked model cache ----
//
// ONNX Runtime packs every MatMulInteger weight into the layout its kernels want (MLAS, per
// instruction set) when a session is created, and keeps the packed copies on the heap: about
// 200 MB of private memory for this model. With session.save_external_prepacked_constant_initializers
// it can instead write the optimized model with the packed weights into an external data file,
// which later sessions map read-only. The layout depends on the CPU and the ORT version, so the
// copy is built on the user's machine on first start and keyed by model file, ORT version and CPU.

Ort::SessionOptions session_options(const RerankerOptions& opt) {
  Ort::SessionOptions so;
  so.SetIntraOpNumThreads(opt.intra_threads);
  so.SetInterOpNumThreads(1);
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  return so;
}

std::string cache_key(const std::filesystem::path& model, const std::filesystem::path& tokenizer) {
  std::error_code ec;
  std::string id;
  for (const auto& file : {model, tokenizer}) {
    id += wide_to_utf8(file.filename().wstring());
    id += '|' + std::to_string(std::filesystem::file_size(file, ec));
    id += '|' + std::to_string(std::filesystem::last_write_time(file, ec).time_since_epoch().count()) + '|';
  }
  id += Ort::GetVersionString();
  int r[4];
  for (int leaf : {static_cast<int>(0x80000002), static_cast<int>(0x80000003), static_cast<int>(0x80000004)}) {
    __cpuid(r, leaf);  // brand string
    id.append(reinterpret_cast<const char*>(r), sizeof(r));
  }
  __cpuidex(r, 7, 0);  // AVX2 / AVX-512 / VNNI / AMX feature bits
  id.append(reinterpret_cast<const char*>(r), sizeof(r));
  __cpuid(r, 1);  // ecx, edx feature bits (ebx holds the APIC id, which differs per core)
  id.append(reinterpret_cast<const char*>(r + 2), 2 * sizeof(int));
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : id) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// Writes <dir>/laya.onnx + laya.data (the prepacked weights) by creating a throw-away session
// that saves its optimized model. Built in a temporary directory and moved into place (laya.onnx
// last: its presence means the cache is complete), so a crash or a full disk never leaves a
// half-written cache behind.
void build_prepacked_model(Ort::Env& env, const RerankerOptions& opt, const std::wstring& model_path,
                           const std::filesystem::path& dir) {
  const std::filesystem::path tmp = dir.wstring() + L".tmp";
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);
  std::filesystem::create_directories(tmp);
  {
    Ort::SessionOptions so = session_options(opt);
    so.SetOptimizedModelFilePath((tmp / L"laya.onnx").c_str());
    so.AddConfigEntry("session.optimized_model_external_initializers_file_name", "laya.data");
    so.AddConfigEntry("session.optimized_model_external_initializers_min_size_in_bytes", "4096");
    so.AddConfigEntry("session.save_external_prepacked_constant_initializers", "1");
    Ort::Session saver(env, model_path.c_str(), so);  // the files are written during construction
  }
  std::filesystem::create_directories(dir);
  std::filesystem::rename(tmp / L"laya.data", dir / L"laya.data");
  std::filesystem::rename(tmp / L"laya.onnx", dir / L"laya.onnx");
  std::filesystem::remove_all(tmp, ec);
}

// Deletes caches for other keys (an older model, ORT version or CPU) under root.
void remove_stale_caches(const std::filesystem::path& root, const std::string& keep) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    if (entry.path().filename().wstring() == utf8_to_wide(keep)) continue;
    std::filesystem::remove_all(entry.path(), ec);
  }
}

}  // namespace

Reranker::Reranker(const RerankerOptions& opt) {
  const std::string tokenizer_path = opt.model_dir + "/tokenizer.json";
  const std::wstring model_path = utf8_to_wide(opt.model_dir + "/" + opt.onnx_file);
  std::filesystem::path root, dir;  // the cache: <cache_dir>/<key>/
  std::string key;
  if (!opt.cache_dir.empty()) {
    root = utf8_to_wide(opt.cache_dir);
    key = cache_key(model_path, utf8_to_wide(tokenizer_path));
    dir = root / utf8_to_wide(key);
  }

  // The tokenizer's compact image: mapped from the cache when present, else built from the JSON
  // (and written there for the next start).
  tok_ = std::make_unique<BpeTokenizer>(tokenizer_path,
                                        dir.empty() ? std::string() : wide_to_utf8((dir / L"tokenizer.bin").wstring()));
  load_note_ = tok_->mapped() ? "tokenizer mapped; " : "tokenizer built from json; ";

  std::ifstream cf(utf8_to_wide(opt.model_dir + "/rl_agent_config.json"));
  if (cf) {
    auto cfg = nlohmann::json::parse(cf);
    max_len_ = cfg.value("max_len", max_len_);
    head_max_len_ = cfg.value("head_max_len", head_max_len_);
    // Our key, not Laya's: models fine-tuned on the guess prompt say so here.
    guess_prompt_ = cfg.value("zhiyi_prompt", std::string()) == "guess";
  }

  env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "laya");
  if (!dir.empty()) {
    const std::filesystem::path cached = dir / L"laya.onnx";
    std::error_code ec;
    try {
      if (!std::filesystem::exists(cached, ec)) {
        const auto t0 = std::chrono::steady_clock::now();
        build_prepacked_model(*env_, opt, model_path, dir);
        load_note_ += "prepacked cache built in " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0).count()) +
                     " ms; ";
      }
      remove_stale_caches(root, key);
      session_ = std::make_unique<Ort::Session>(*env_, cached.c_str(), session_options(opt));
      load_note_ += "prepacked weights mapped from " + wide_to_utf8(cached.wstring());
    } catch (const std::exception& e) {
      // Unwritable cache directory, full disk, or a cache ORT rejects: use the model as it is.
      session_.reset();
      std::filesystem::remove(cached, ec);
      std::filesystem::remove(dir / L"laya.data", ec);
      load_note_ += std::string("prepacked cache unusable (") + e.what() + "); ";
    }
  }
  if (!session_) {
    session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options(opt));
    load_note_ += "weights packed on the heap";
  }
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
