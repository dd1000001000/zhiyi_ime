// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "reranker.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <json.hpp>  // cxx-ime vendors nlohmann/json as third_party/nlohmann/json.hpp
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include "text_util.h"

namespace laya {

const char* const kInstructionsGuess = "Which candidate word most likely comes next after the text so far?";

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

// DirectML (laya.device): one fixed shape (see RerankerOptions::gpu_device). The DirectML
// provider wants no memory pattern and sequential execution.
Ort::SessionOptions gpu_session_options(int device) {
  Ort::SessionOptions so;
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  so.DisableMemPattern();
  so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  so.AddFreeDimensionOverrideByName("batch_size", 1);
  so.AddFreeDimensionOverrideByName("seq_len", kGpuSequenceLength);
  so.AddFreeDimensionOverrideByName("num_markers", kGpuMarkers);
  so.AddFreeDimensionOverrideByName("num_slots", kGpuSlots);
  const OrtDmlApi* dml = nullptr;
  Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                          reinterpret_cast<const void**>(&dml)));
  if (!dml) throw std::runtime_error("ONNX Runtime has no DirectML provider");
  Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(so, device));
  return so;
}

std::vector<char> read_file(const std::wstring& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) throw std::runtime_error("cannot read the model file");
  return std::vector<char>(std::istreambuf_iterator<char>(in), {});
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

bool has_input(const Ort::Session& session, const char* name) {
  Ort::AllocatorWithDefaultOptions alloc;
  for (size_t i = 0, n = session.GetInputCount(); i < n; ++i) {
    if (std::string(session.GetInputNameAllocated(i, alloc).get()) == name) return true;
  }
  return false;
}

bool has_output(const Ort::Session& session, const char* name) {
  Ort::AllocatorWithDefaultOptions alloc;
  for (size_t i = 0, n = session.GetOutputCount(); i < n; ++i) {
    if (std::string(session.GetOutputNameAllocated(i, alloc).get()) == name) return true;
  }
  return false;
}

// ln softmax of x over the characters `ok` allows (-1e4 for the others).
void masked_ln_softmax(const float* x, size_t C, const std::vector<uint8_t>& ok, std::vector<float>& lp) {
  const size_t n = std::min(C, ok.size());
  float mx = -1e30f;
  for (size_t k = 0; k < n; ++k)
    if (ok[k] && x[k] > mx) mx = x[k];
  double sum = 0.0;
  for (size_t k = 0; k < n; ++k)
    if (ok[k]) sum += std::exp(static_cast<double>(x[k] - mx));
  const float lse = mx + static_cast<float>(std::log((std::max)(sum, 1e-30)));
  lp.assign(C, -1e4f);
  for (size_t k = 0; k < n; ++k)
    if (ok[k]) lp[k] = x[k] - lse;
}

}  // namespace

size_t clear_reshape_allowzero(std::vector<char>& model) {
  // AttributeProto: name (field 1) = "allowzero", then i (field 3, varint) = 1.
  static const char kPattern[] = {0x0a, 0x09, 'a', 'l', 'l', 'o', 'w', 'z', 'e', 'r', 'o', 0x18, 0x01};
  const size_t n = sizeof(kPattern);
  size_t cleared = 0;
  for (auto it = model.begin();
       (it = std::search(it, model.end(), kPattern, kPattern + n)) != model.end(); it += n) {
    *(it + n - 1) = 0;
    ++cleared;
  }
  return cleared;
}

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
  }

  env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "laya");
  env_->DisableTelemetryEvents();
  // A unified model (unified_vocab.json beside it) is not served from the prepacked cache:
  // ONNX Runtime (1.23 / 1.24) saves its prepacked weights but the saved graph then crashes on
  // its first run (bench export_unified.py: the continuation head's layers with an int8
  // embedding Gather). Its weights are packed on the heap instead (~100 MB of private memory).
  std::error_code vocab_ec;
  const bool unified_files = std::filesystem::exists(
      std::filesystem::path(utf8_to_wide(opt.model_dir)) / L"unified_vocab.json", vocab_ec);
  if (opt.gpu_device >= 0) {
    // The prepacked cache is a CPU layout; the card gets the model as it is, patched in memory.
    std::vector<char> model = read_file(model_path);
    clear_reshape_allowzero(model);
    session_ = std::make_unique<Ort::Session>(*env_, model.data(), model.size(),
                                              gpu_session_options(opt.gpu_device));
    gpu_ = true;
    max_len_ = (std::min)(max_len_, kGpuSequenceLength);
    load_note_ += "DirectML device " + std::to_string(opt.gpu_device);
  } else if (!dir.empty() && unified_files) {
    load_note_ += "prepacked cache not used (unified model); ";
  } else if (!dir.empty()) {
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

  // A unified model reads the typed input as slots; its vocabulary (units, characters) comes
  // with it. A model without that input is the plain choice model.
  slots_input_ = has_input(*session_, "unit_ids");
  if (slots_input_) {
    vocab_ = std::make_unique<UnifiedVocab>(opt.model_dir + "/unified_vocab.json");
    context_prefix_ids_ = tok_->encode(vocab_->context_prefix());
    load_note_ += "; unified model (" + std::to_string(vocab_->char_count()) + " characters)";
    // A decoder model: its decoder graph beside it, on the CPU (a few small calls per character).
    memory_output_ = has_output(*session_, "memory");
    if (memory_output_ && !vocab_->decoder_file().empty()) {
      const std::wstring decoder_path = utf8_to_wide(opt.model_dir + "/" + vocab_->decoder_file());
      std::error_code dec_ec;
      if (std::filesystem::exists(std::filesystem::path(decoder_path), dec_ec)) {
        decoder_ = std::make_unique<Ort::Session>(*env_, decoder_path.c_str(), session_options(opt));
        load_note_ += "; decoder " + vocab_->decoder_file();
        const std::string& memory_file = vocab_->decoder_memory_file();
        const std::string& step_file = vocab_->decoder_step_file();
        const std::wstring memory_path = utf8_to_wide(opt.model_dir + "/" + memory_file);
        const std::wstring step_path = utf8_to_wide(opt.model_dir + "/" + step_file);
        if (!memory_file.empty() && !step_file.empty() &&
            std::filesystem::exists(std::filesystem::path(memory_path), dec_ec) &&
            std::filesystem::exists(std::filesystem::path(step_path), dec_ec)) {
          decoder_memory_ = std::make_unique<Ort::Session>(*env_, memory_path.c_str(), session_options(opt));
          decoder_step_ = std::make_unique<Ort::Session>(*env_, step_path.c_str(), session_options(opt));
          load_note_ += " (KV cache: " + memory_file + ", " + step_file + ")";
        }
      } else {
        load_note_ += "; decoder file missing: " + vocab_->decoder_file();
      }
    }
  }

  head_ids_ = tok_->encode(std::string("choice question: ") +
                           replace_all(kInstructionsGuess, tok_->mask_token(), " "));
  // A card that loads the model can still fail to run it (driver, unsupported operator): find
  // out now, while the caller can still fall back to the CPU. Also compiles the graph.
  if (gpu_ && score("", "", {"a", "b"}).size() != 2) throw std::runtime_error("DirectML run failed");
}

Reranker::~Reranker() = default;

Sequence Reranker::build(const std::string& context, const std::string& pinyin,
                         const std::vector<std::string>& candidates, Task task) const {
  std::vector<Unit> units;
  if (task == Task::kEnglish && vocab_) units = letter_units(pinyin, static_cast<size_t>(vocab_->max_slots()));
  return build(context, candidates, units);
}

Sequence Reranker::build(const std::string& context, const std::vector<std::string>& candidates,
                         const std::vector<Unit>& units) const {
  const std::vector<int64_t>& head_ids = head_ids_;
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

  // build_sequence: 状态放在选项之后, 按剩余空间截断, 最后补 [SEP]。The slots that follow
  // (unified_common.Encoder.choice) take their own room: the state is cut to leave it.
  const size_t slot_room = vocab_ && !units.empty()
                               ? std::min(units.size(), static_cast<size_t>(vocab_->max_slots())) + 1
                               : 0;
  auto state_ids = tok.encode(replace_all(guess_state(context), mask, " "));
  long room = std::max<long>(0, static_cast<long>(max_len_) - static_cast<long>(seq.ids.size()) - 1 -
                                    static_cast<long>(slot_room));
  if (static_cast<long>(state_ids.size()) > room) state_ids.resize(static_cast<size_t>(room));
  seq.ids.insert(seq.ids.end(), state_ids.begin(), state_ids.end());
  seq.ids.push_back(tok.sep_id());
  if (seq.ids.size() > static_cast<size_t>(max_len_)) seq.ids.resize(max_len_);
  seq.markers.erase(std::remove_if(seq.markers.begin(), seq.markers.end(),
                                   [&](int64_t m) { return m >= max_len_; }),
                    seq.markers.end());
  if (slot_room) append_slots(seq, units);
  return seq;
}

void Reranker::append_slots(Sequence& seq, const std::vector<Unit>& units) const {
  seq.units.assign(seq.ids.size(), 0);
  const size_t n = std::min(units.size(), static_cast<size_t>(vocab_->max_slots()));
  for (size_t i = 0; i < n; ++i) {
    seq.slots.push_back(static_cast<int64_t>(seq.ids.size()));
    seq.ids.push_back(tok_->mask_id());
    seq.units.push_back(vocab_->unit_id(units[i]));
  }
  seq.ids.push_back(tok_->sep_id());
  seq.units.push_back(0);
}

std::vector<float> Reranker::score(const std::string& context, const std::string& pinyin,
                                   const std::vector<std::string>& candidates, Task task) const {
  std::vector<Unit> units;
  if (task == Task::kEnglish && vocab_) units = letter_units(pinyin, static_cast<size_t>(vocab_->max_slots()));
  return score(context, candidates, units);
}

std::vector<float> Reranker::score(const std::string& context, const std::vector<std::string>& candidates,
                                   const std::vector<Unit>& units) const {
  if (candidates.empty()) return {};
  Sequence seq = build(context, candidates, units);
  // The card's fixed length: drop the oldest context until the sequence fits (build() would cut
  // the newest text, right before the caret, instead).
  for (size_t chars = utf8_length(context); gpu_ && seq.ids.size() >= static_cast<size_t>(max_len_) && chars > 0;) {
    chars -= (std::min)(chars, (std::max)(size_t{8}, chars / 4));
    seq = build(utf8_tail(context, chars), candidates, units);
  }
  return score(seq);
}

bool Reranker::run(Sequence seq, std::vector<float>* logits, std::vector<float>* char_logits,
                   size_t* chars, Encoded* encoded) const {
  const int64_t used = static_cast<int64_t>(seq.ids.size());
  const int64_t K = static_cast<int64_t>(seq.markers.size());
  const int64_t S = static_cast<int64_t>(seq.slots.size());
  if (gpu_ && (used > kGpuSequenceLength || K > kGpuMarkers || S > kGpuSlots)) return false;
  // The card's fixed shape: tokens padded with id 0 and attention 0, extra candidates masked
  // (their logits are not read), extra slots at position 0 (not read either).
  const int64_t L = gpu_ ? kGpuSequenceLength : used;
  const int64_t markers = gpu_ ? kGpuMarkers : (std::max)(K, int64_t{1});
  const int64_t slots = gpu_ ? kGpuSlots : (std::max)(S, int64_t{1});
  std::vector<int64_t> ids = seq.ids, att(used, 1), pos = seq.markers, qtype{0};  // 0 = choice
  std::vector<int64_t> units = seq.units, slot_pos = seq.slots;
  ids.resize(L, 0);
  att.resize(L, 0);
  units.resize(L, 0);
  pos.resize(markers, 0);
  slot_pos.resize(slots, 0);
  std::unique_ptr<bool[]> mmask(new bool[markers]);
  std::fill(mmask.get(), mmask.get() + markers, false);
  std::fill(mmask.get(), mmask.get() + K, true);

  const int64_t s_ids[2] = {1, L}, s_mark[2] = {1, markers}, s_slot[2] = {1, slots}, s_q[1] = {1};
  std::vector<Ort::Value> in;
  std::vector<const char*> in_names;
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, ids.data(), ids.size(), s_ids, 2));
  in_names.push_back("input_ids");
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, att.data(), att.size(), s_ids, 2));
  in_names.push_back("attention_mask");
  if (slots_input_) {
    in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, units.data(), units.size(), s_ids, 2));
    in_names.push_back("unit_ids");
  }
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, pos.data(), pos.size(), s_mark, 2));
  in_names.push_back("marker_pos");
  in.push_back(Ort::Value::CreateTensor<bool>(*mem_, mmask.get(), markers, s_mark, 2));
  in_names.push_back("marker_mask");
  if (slots_input_) {
    in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, slot_pos.data(), slot_pos.size(), s_slot, 2));
    in_names.push_back("slot_pos");
  } else {
    in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, qtype.data(), 1, s_q, 1));
    in_names.push_back("qtype");
  }
  std::vector<const char*> out_names;
  if (logits) out_names.push_back("logits");
  if (char_logits && slots_input_) out_names.push_back("char_logits");
  if (encoded && memory_output_) {
    out_names.push_back("memory");
    out_names.push_back("slot_vec");
  }
  if (out_names.empty()) return false;
  std::unique_lock<std::mutex> gpu_lock;
  if (gpu_) gpu_lock = std::unique_lock<std::mutex>(gpu_run_mutex_);
  auto out = session_->Run(Ort::RunOptions{nullptr}, in_names.data(), in.data(), in.size(), out_names.data(),
                           out_names.size());
  size_t next = 0;
  if (logits) {
    const float* l = out[next++].GetTensorData<float>();
    logits->assign(l, l + K);
  }
  if (char_logits && slots_input_) {
    const Ort::Value& v = out[next++];
    const auto shape = v.GetTensorTypeAndShapeInfo().GetShape();  // (1, slots, chars)
    const size_t C = shape.size() == 3 ? static_cast<size_t>(shape[2]) : 0;
    const float* c = v.GetTensorData<float>();
    char_logits->assign(c, c + static_cast<size_t>(S) * C);
    if (chars) *chars = C;
  }
  if (encoded && memory_output_) {
    const Ort::Value& m = out[next++];
    const auto mshape = m.GetTensorTypeAndShapeInfo().GetShape();  // (1, L, width)
    const Ort::Value& sv = out[next++];
    const auto sshape = sv.GetTensorTypeAndShapeInfo().GetShape();  // (1, slots, width)
    if (mshape.size() != 3 || sshape.size() != 3 || mshape[1] != L || sshape[1] < S) return false;
    encoded->length = L;
    encoded->width = mshape[2];
    encoded->slots = S;
    const float* md = m.GetTensorData<float>();
    encoded->memory.assign(md, md + static_cast<size_t>(L * mshape[2]));
    encoded->mask = att;
    const float* sd = sv.GetTensorData<float>();
    encoded->slot_vec.assign(sd, sd + static_cast<size_t>(S * sshape[2]));
    encoded->unit_ids.clear();
    for (int64_t j = 0; j < S; ++j)
      encoded->unit_ids.push_back(seq.units[static_cast<size_t>(seq.slots[static_cast<size_t>(j)])]);
  }
  return true;
}

std::vector<float> Reranker::score(const Sequence& seq) const {
  const size_t K = seq.markers.size();
  if (K == 0) return {};
  std::vector<float> p;
  if (!run(seq, &p, nullptr, nullptr, nullptr) || p.size() != K) return {};
  float mx = *std::max_element(p.begin(), p.end());
  float sum = 0;
  for (auto& v : p) sum += (v = std::exp(v - mx));
  for (auto& v : p) v /= sum;
  return p;
}

Sequence Reranker::continuation_sequence(const std::string& context, const std::vector<Unit>& units) const {
  const auto& tok = *tok_;
  // A long context keeps the prefix and the text nearest the caret.
  Sequence seq;
  auto ctx = tok.encode(vocab_->context_prefix() + context);
  const size_t max_ctx = static_cast<size_t>(vocab_->max_context_tokens());
  if (ctx.size() > max_ctx) {
    const size_t keep = max_ctx - std::min(max_ctx, context_prefix_ids_.size());
    std::vector<int64_t> cut(context_prefix_ids_);
    cut.insert(cut.end(), ctx.end() - static_cast<std::ptrdiff_t>(keep), ctx.end());
    ctx = std::move(cut);
  }
  seq.ids.push_back(tok.cls_id());
  seq.ids.insert(seq.ids.end(), ctx.begin(), ctx.end());
  seq.ids.push_back(tok.sep_id());
  append_slots(seq, units);
  return seq;
}

std::vector<std::vector<float>> Reranker::continuation(const std::string& context,
                                                       const std::vector<Unit>& units) const {
  if (!vocab_ || units.empty() || units.size() > static_cast<size_t>(vocab_->max_slots())) return {};
  const Sequence seq = continuation_sequence(context, units);
  std::vector<float> raw;
  size_t C = 0;
  if (!run(seq, nullptr, &raw, &C, nullptr) || C == 0) return {};
  std::vector<std::vector<float>> out(seq.slots.size());
  for (size_t j = 0; j < seq.slots.size(); ++j) {
    // ln softmax over the characters the unit can spell (the others are never predicted).
    masked_ln_softmax(raw.data() + j * C, C, vocab_->allowed(seq.units[static_cast<size_t>(seq.slots[j])]), out[j]);
  }
  return out;
}

std::shared_ptr<const Encoded> Reranker::encode(const std::string& context, const std::vector<Unit>& units) const {
  if (!decoder_ || !vocab_ || units.empty() || units.size() > static_cast<size_t>(vocab_->max_slots())) return nullptr;
  const Sequence seq = continuation_sequence(context, units);
  auto encoded = std::make_shared<Encoded>();
  if (!run(seq, nullptr, nullptr, nullptr, encoded.get())) return nullptr;
  return encoded;
}

namespace {

// The first n characters of a path, as a cache key.
std::string path_key(const std::vector<int>& chars, size_t n) {
  return std::string(reinterpret_cast<const char*>(chars.data()), n * sizeof(int));
}

}  // namespace

std::vector<std::vector<float>> Reranker::decode_cached(const Encoded& enc,
                                                       const std::vector<std::vector<int>>& prefixes, size_t slot,
                                                       DecoderCache& cache) const {
  const int64_t B = static_cast<int64_t>(prefixes.size()), L = enc.length, d = enc.width;
  const int64_t t = static_cast<int64_t>(slot);
  for (const auto& p : prefixes) {
    if (p.size() < slot) return {};
  }
  if (cache.cross_shape.empty()) {
    const int64_t s_mem[3] = {1, L, d};
    Ort::Value in = Ort::Value::CreateTensor<float>(*mem_, const_cast<float*>(enc.memory.data()),
                                                    enc.memory.size(), s_mem, 3);
    const char* in_names[1] = {"memory"};
    const char* out_names[2] = {"cross_k", "cross_v"};
    auto out = decoder_memory_->Run(Ort::RunOptions{nullptr}, in_names, &in, 1, out_names, 2);
    const auto info = out[0].GetTensorTypeAndShapeInfo();
    if (info.GetShape().size() != 4) return {};
    const size_t n = info.GetElementCount();
    cache.cross_k.assign(out[0].GetTensorData<float>(), out[0].GetTensorData<float>() + n);
    cache.cross_v.assign(out[1].GetTensorData<float>(), out[1].GetTensorData<float>() + n);
    cache.cross_shape = info.GetShape();
  }
  if (t == 0) {
    cache.paths.clear();
    cache.positions = 0;
  }
  if (cache.positions != t) return {};  // not the slot after the last call's
  const int64_t NL = cache.cross_shape[0], H = cache.cross_shape[1], dh = cache.cross_shape[3];
  const size_t row = static_cast<size_t>(NL * H * t * dh);
  std::vector<float> past_k(static_cast<size_t>(B) * row), past_v(static_cast<size_t>(B) * row);
  std::vector<int64_t> prev(static_cast<size_t>(B), -1);
  for (int64_t b = 0; b < B; ++b) {
    if (t == 0) break;
    const std::vector<int>& p = prefixes[static_cast<size_t>(b)];
    const auto it = cache.paths.find(path_key(p, slot - 1));
    if (it == cache.paths.end() || it->second.first.size() != row) return {};
    std::copy(it->second.first.begin(), it->second.first.end(), past_k.begin() + static_cast<std::ptrdiff_t>(b * row));
    std::copy(it->second.second.begin(), it->second.second.end(), past_v.begin() + static_cast<std::ptrdiff_t>(b * row));
    prev[static_cast<size_t>(b)] = p[slot - 1];
  }
  float empty = 0.0f;  // the first slot has no keys before it: a tensor of 0 elements
  const int64_t s_mask[2] = {1, L}, s_slot[2] = {1, d}, s_prev[1] = {B}, s_past[5] = {B, NL, H, t, dh};
  std::vector<Ort::Value> in;
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, cache.cross_k.data(), cache.cross_k.size(),
                                               cache.cross_shape.data(), 4));
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, cache.cross_v.data(), cache.cross_v.size(),
                                               cache.cross_shape.data(), 4));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, const_cast<int64_t*>(enc.mask.data()), enc.mask.size(),
                                                 s_mask, 2));
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, const_cast<float*>(enc.slot_vec.data()) + t * d,
                                               static_cast<size_t>(d), s_slot, 2));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, prev.data(), prev.size(), s_prev, 1));
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, past_k.empty() ? &empty : past_k.data(), past_k.size(),
                                               s_past, 5));
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, past_v.empty() ? &empty : past_v.data(), past_v.size(),
                                               s_past, 5));
  const char* in_names[7] = {"cross_k", "cross_v", "memory_mask", "slot_vec", "prev", "past_k", "past_v"};
  const char* out_names[3] = {"char_logits", "present_k", "present_v"};
  auto out = decoder_step_->Run(Ort::RunOptions{nullptr}, in_names, in.data(), in.size(), out_names, 3);
  const auto shape = out[0].GetTensorTypeAndShapeInfo().GetShape();  // (B, chars)
  const auto present = out[1].GetTensorTypeAndShapeInfo().GetShape();  // (B, layers, heads, t + 1, head_dim)
  if (shape.size() != 2 || shape[0] != B || present.size() != 5 || present[3] != t + 1) return {};
  const size_t C = static_cast<size_t>(shape[1]);
  const size_t next_row = static_cast<size_t>(NL * H * (t + 1) * dh);
  const float* data = out[0].GetTensorData<float>();
  const float* pk = out[1].GetTensorData<float>();
  const float* pv = out[2].GetTensorData<float>();
  std::unordered_map<std::string, std::pair<std::vector<float>, std::vector<float>>> paths;
  const std::vector<uint8_t>& ok = vocab_->allowed(enc.unit_ids[slot]);
  std::vector<std::vector<float>> result(static_cast<size_t>(B));
  for (int64_t b = 0; b < B; ++b) {
    auto& kept = paths[path_key(prefixes[static_cast<size_t>(b)], slot)];
    if (kept.first.empty()) {
      kept.first.assign(pk + b * next_row, pk + (b + 1) * next_row);
      kept.second.assign(pv + b * next_row, pv + (b + 1) * next_row);
    }
    masked_ln_softmax(data + static_cast<size_t>(b) * C, C, ok, result[static_cast<size_t>(b)]);
  }
  cache.paths = std::move(paths);
  cache.positions = t + 1;
  return result;
}

std::vector<std::vector<float>> Reranker::decode(const Encoded& enc, const std::vector<std::vector<int>>& prefixes,
                                                 size_t slot, DecoderCache* cache) const {
  if (!decoder_ || !vocab_ || prefixes.empty() || static_cast<int64_t>(slot) >= enc.slots || enc.width == 0) return {};
  if (cache && has_decoder_cache()) {
    auto result = decode_cached(enc, prefixes, slot, *cache);
    if (result.size() == prefixes.size()) return result;
    cache->positions = -1;  // out of step: the rest of this reading without the cache
  }
  const int64_t B = static_cast<int64_t>(prefixes.size()), L = enc.length, d = enc.width;
  const int64_t T = static_cast<int64_t>(slot) + 1;
  // The reading repeated for every prefix (the decoder graph wants matching batches).
  std::vector<float> memory(static_cast<size_t>(B * L * d)), slot_vec(static_cast<size_t>(B * T * d));
  std::vector<int64_t> mask(static_cast<size_t>(B * L)), prev(static_cast<size_t>(B * T), -1);
  for (int64_t b = 0; b < B; ++b) {
    std::copy(enc.memory.begin(), enc.memory.end(), memory.begin() + static_cast<std::ptrdiff_t>(b * L * d));
    std::copy(enc.mask.begin(), enc.mask.end(), mask.begin() + static_cast<std::ptrdiff_t>(b * L));
    std::copy(enc.slot_vec.begin(), enc.slot_vec.begin() + static_cast<std::ptrdiff_t>(T * d),
              slot_vec.begin() + static_cast<std::ptrdiff_t>(b * T * d));
    const std::vector<int>& p = prefixes[static_cast<size_t>(b)];
    for (int64_t t = 1; t < T; ++t) {
      if (static_cast<size_t>(t - 1) < p.size()) prev[static_cast<size_t>(b * T + t)] = p[static_cast<size_t>(t - 1)];
    }
  }
  const int64_t s_mem[3] = {B, L, d}, s_mask[2] = {B, L}, s_slot[3] = {B, T, d}, s_prev[2] = {B, T};
  std::vector<Ort::Value> in;
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, memory.data(), memory.size(), s_mem, 3));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, mask.data(), mask.size(), s_mask, 2));
  in.push_back(Ort::Value::CreateTensor<float>(*mem_, slot_vec.data(), slot_vec.size(), s_slot, 3));
  in.push_back(Ort::Value::CreateTensor<int64_t>(*mem_, prev.data(), prev.size(), s_prev, 2));
  const char* in_names[4] = {"memory", "memory_mask", "slot_vec", "prev"};
  const char* out_names[1] = {"char_logits"};
  auto out = decoder_->Run(Ort::RunOptions{nullptr}, in_names, in.data(), in.size(), out_names, 1);
  const auto shape = out[0].GetTensorTypeAndShapeInfo().GetShape();  // (B, T, chars)
  if (shape.size() != 3 || shape[0] != B || shape[1] != T) return {};
  const size_t C = static_cast<size_t>(shape[2]);
  const float* data = out[0].GetTensorData<float>();
  const std::vector<uint8_t>& ok = vocab_->allowed(enc.unit_ids[slot]);
  std::vector<std::vector<float>> result(static_cast<size_t>(B));
  for (int64_t b = 0; b < B; ++b) {
    masked_ln_softmax(data + static_cast<size_t>(b * T + static_cast<int64_t>(slot)) * C, C, ok,
                      result[static_cast<size_t>(b)]);
  }
  return result;
}

}  // namespace laya
