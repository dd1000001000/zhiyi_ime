// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/laya_rerank.h>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <cxxime/config.h>
#include <cxxime/english_lexicon.h>
#include <cxxime/gpu_adapters.h>
#include <cxxime/segmentor.h>
#include <cxxime/translation_result.h>

#include "laya/reranker.h"
#include "laya/text_util.h"

namespace cxxime {

namespace {

// Always-on (also in Release): the model is the one component most likely to be missing or
// broken on a user's machine. View with DebugView / a debugger.
void laya_log(const std::wstring& message) {
    OutputDebugStringW((L"[ZhiyiIME][laya] " + message + L"\n").c_str());
}

struct State {
    std::mutex mu;
    std::shared_ptr<laya::Reranker> model;
    std::string model_key;
    bool loading = false;
    bool failed = false;
    std::unordered_map<std::string, std::vector<size_t>> cache;  // context|pinyin|cands -> order
    LayaRerankStats stats;
    LayaRerank::CaptureFn capture;
};

// Intentionally leaked: the detached loader thread may still touch it during process exit.
State& state() {
    static State* s = new State;
    return *s;
}

std::string resolve_model_dir(const std::string& dir) {
    std::filesystem::path p(laya::utf8_to_wide(dir));
    if (p.is_relative()) {
        wchar_t buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        p = std::filesystem::path(std::wstring(buf, n)).parent_path() / p;
    }
    return laya::wide_to_utf8(p.lexically_normal().wstring());
}

// %LOCALAPPDATA%\zhiyi\laya-cache: machine-specific (the packed layout depends on the CPU), so
// neither the roaming profile nor the user data directory that users back up.
std::string laya_cache_dir() {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf) != S_OK) return {};
    return laya::wide_to_utf8(std::wstring(buf) + L"\\zhiyi\\laya-cache");
}

laya::RerankerOptions model_options(const Config& config) {
    laya::RerankerOptions opt;
    opt.model_dir = resolve_model_dir(config.laya.model_dir);
    opt.onnx_file = config.laya.onnx;
    opt.intra_threads = (std::max)(1, config.laya.threads);
    if (config.laya.cache) opt.cache_dir = laya_cache_dir();
    return opt;
}

// onnxruntime.dll is delay-loaded: make sure it and the model exist before any ONNX Runtime
// call (otherwise the delay-load helper raises an SEH exception). Returns what is missing.
std::wstring missing_model_file(const laya::RerankerOptions& opt) {
    const std::filesystem::path dir(laya::utf8_to_wide(opt.model_dir));
    for (const std::string& f : {std::string("tokenizer.json"), std::string("rl_agent_config.json"), opt.onnx_file}) {
        std::error_code ec;
        if (!std::filesystem::exists(dir / laya::utf8_to_wide(f), ec)) return laya::utf8_to_wide(f);
    }
    if (!LoadLibraryExW(L"onnxruntime.dll", nullptr,
                        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        return L"onnxruntime.dll";
    return {};
}

// The model on the graphics card called `name`; nullptr (and a log line) when the card is
// missing or fails.
std::shared_ptr<laya::Reranker> load_on_gpu(laya::RerankerOptions opt, const std::string& name,
                                            std::string* error = nullptr) {
    const std::vector<GpuAdapter> adapters = list_gpu_adapters();
    const GpuAdapter* adapter = find_gpu_adapter(adapters, name);
    if (!adapter) {
        laya_log(L"graphics card " + laya::utf8_to_wide(name) + L" not found");
        if (error) *error = "graphics card not found";
        return nullptr;
    }
    opt.gpu_device = static_cast<int>(adapter->index);
    try {
        return std::make_shared<laya::Reranker>(opt);
    } catch (const std::exception& e) {
        laya_log(L"graphics card " + laya::utf8_to_wide(name) + L" failed: " + laya::utf8_to_wide(e.what()));
        if (error) *error = e.what();
        return nullptr;
    }
}

// Returns the shared model, starting a background load on first use. nullptr until ready.
std::shared_ptr<laya::Reranker> get_model(const Config& config) {
    laya::RerankerOptions opt = model_options(config);
    const std::string device = config.laya.device;
    const std::string key = opt.model_dir + "|" + opt.onnx_file + "|" + device;
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.model_key != key) {  // first use, or the configured model or device changed
        s.model_key = key;
        s.model.reset();
        s.failed = false;
        s.cache.clear();
    }
    if (s.model || s.loading || s.failed) return s.model;
    // Checked here, before a loader thread exists, so processes without a model (tests, tools)
    // never start one.
    const std::wstring missing = missing_model_file(opt);
    if (!missing.empty()) {
        laya_log(L"disabled: " + missing + L" not found (model dir " + laya::utf8_to_wide(opt.model_dir) + L")");
        s.failed = true;
        s.stats.model_failed = true;
        return nullptr;
    }
    s.loading = true;
    std::thread([opt, key, device] {
        std::shared_ptr<laya::Reranker> model;
        auto t0 = std::chrono::steady_clock::now();
        // A card that is missing or fails falls back to the CPU.
        if (!device.empty()) model = load_on_gpu(opt, device);
        try {
            if (!model) model = std::make_shared<laya::Reranker>(opt);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            laya_log(L"model loaded from " + laya::utf8_to_wide(opt.model_dir) + L" in " +
                     std::to_wstring(ms) + L" ms (" + laya::utf8_to_wide(model->load_note()) + L")");
        } catch (const std::exception& e) {
            laya_log(L"failed to load model from " + laya::utf8_to_wide(opt.model_dir) + L": " +
                     laya::utf8_to_wide(e.what()));
        }
        State& st = state();
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.model_key != key) return;  // configuration changed while loading
        st.loading = false;
        st.model = model;
        st.failed = !model;
        st.stats.model_ready = static_cast<bool>(model);
        st.stats.model_failed = !model;
        st.stats.on_gpu = model && model->on_gpu();
    }).detach();
    return nullptr;
}

// One typical first page for the speed test (about 200 tokens): 100 characters of context and
// 20 candidates. Median of 15 runs after 3 warm-up runs; -1 when the model fails.
double median_score_ms(const laya::Reranker& model) {
    const std::string context =
        u8"今天下午我们在会议室讨论了下一个版本的计划，大家都觉得输入法的候选还可以更准确一些，"
        u8"尤其是同音词比较多的时候，所以决定先收集一些常见的例子，再看看模型在这些句子上的表现，然后";
    const std::vector<std::string> candidates = {
        u8"权利", u8"权力", u8"全力", u8"拳力", u8"泉里", u8"全立", u8"权利人", u8"权力机关", u8"全力以赴",
        u8"权利义务", u8"犬类", u8"劝离", u8"全理", u8"权例", u8"全例", u8"权立", u8"泉力", u8"券里",
        u8"圈里", u8"全利"};
    std::vector<double> times;
    try {
        for (int i = 0; i < 18; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            if (model.score(context, "quan'li", candidates).size() != candidates.size()) return -1.0;
            if (i >= 3)
                times.push_back(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0).count());
        }
    } catch (const std::exception& e) {
        laya_log(L"speed test failed: " + laya::utf8_to_wide(e.what()));
        return -1.0;
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// "jintian" -> "jin'tian", the format the model was trained with.
std::string pinyin_for_model(const std::string& raw) {
    std::string letters;
    for (char c : raw)
        if (c >= 'a' && c <= 'z') letters += c;
    thread_local PinyinSegmentor segmentor;
    std::vector<std::string> syllables = segmentor.segment_best(letters);
    if (syllables.empty()) return letters;
    std::string out;
    for (const auto& s : syllables) {
        if (!out.empty()) out += '\'';
        out += s;
    }
    return out;
}

const TextSelectionAction* text_action(const CandidateEntry& entry) {
    return std::get_if<TextSelectionAction>(&entry.selection);
}

}  // namespace

LayaRerank& LayaRerank::instance() {
    static LayaRerank* r = new LayaRerank;
    return *r;
}

void LayaRerank::preload(const Config& config) {
    if (config.laya.enable) get_model(config);
}

void LayaRerank::set_capture(CaptureFn capture) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    s.capture = std::move(capture);
}

LayaGpuTest LayaRerank::test_gpu(const Config& config, const std::string& adapter) {
    LayaGpuTest result;
    laya::RerankerOptions opt = model_options(config);
    const std::wstring missing = missing_model_file(opt);
    if (!missing.empty()) {
        result.error = "missing " + laya::wide_to_utf8(missing);
        return result;
    }
    {
        std::shared_ptr<laya::Reranker> gpu = load_on_gpu(opt, adapter, &result.error);
        if (!gpu) return result;
        result.gpu_ms = median_score_ms(*gpu);
    }  // the card's memory is released before the CPU copy is loaded
    if (result.gpu_ms < 0) {
        result.error = "the model does not run on this graphics card";
        return result;
    }
    try {
        laya::Reranker cpu(opt);
        result.cpu_ms = median_score_ms(cpu);
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
    result.faster = result.cpu_ms > 0 && result.gpu_ms < result.cpu_ms;
    laya_log(L"speed test on " + laya::utf8_to_wide(adapter) + L": " + std::to_wstring(result.gpu_ms) +
             L" ms, CPU " + std::to_wstring(result.cpu_ms) + L" ms");
    return result;
}

LayaRerankStats LayaRerank::stats() const {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.stats;
}

namespace {

// ln P(the right word is the r-th candidate the model compares), measured on the training data
// (bench: eng_train.jsonl for Chinese, en192_train.jsonl for English). The model is trained on
// shuffled lists and has no position preference of its own; the engine's order (frequency,
// spelling credibility) comes back through this prior, weighted by laya.rank_prior_weight.
constexpr double kRankPriorZh[] = {-0.2615, -2.3137, -3.1248, -3.5859, -4.0583, -4.4188, -4.6940,
                                   -4.9568, -5.0688, -5.2175, -7.1806, -7.2324, -7.7867, -7.7867};
constexpr double kRankPriorEn[] = {-0.3147, -2.1045, -3.0250, -3.4286, -3.8235, -4.1113, -3.9639,
                                   -4.6219};

template <size_t N>
double rank_prior(const double (&table)[N], size_t rank) {
    return table[(std::min)(rank, N - 1)];
}

}  // namespace

bool LayaRerank::apply(const Config& config, const std::string& context, const std::string& input,
                       TranslationResult& result) {
    const auto& lc = config.laya;
    if (!lc.enable || result.entries.size() < 2) return false;

    // 1. The candidates covering the whole input, any length (显示 / 西安市, 今天 / 今天是).
    //    Candidates for part of the input (飘 for "piaol") keep their places.
    // The model compares the first laya_candidate_count() candidates (2 * page_size by default;
    // the engine fetches that many for the first page and shows page_size of them).
    const size_t limit = (std::min)(result.entries.size(),
                                    static_cast<size_t>((std::max)(2, config.laya_candidate_count())));
    std::vector<size_t> idx;
    for (size_t i = 0; i < limit; ++i) {
        const TextSelectionAction* a = text_action(result.entries[i]);
        if (a && a->consumed_input_bytes == input.size() && !result.entries[i].candidate.text.empty())
            idx.push_back(i);
    }
    if (static_cast<int>(idx.size()) < (std::max)(2, lc.min_candidates)) return false;

    // 2. Score with the model (cached).
    const std::string pinyin = pinyin_for_model(input);
    const std::string ctx = laya::utf8_tail(context, static_cast<size_t>((std::max)(0, lc.context_chars)));
    std::vector<std::string> texts;
    std::string key = ctx + '\x1f' + pinyin + '\x1f' + std::to_string(lc.rank_prior_weight);
    for (size_t i : idx) {
        texts.push_back(result.entries[i].candidate.text);
        key += '\x1f' + texts.back();
    }

    State& s = state();
    {
        CaptureFn capture;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            capture = s.capture;
        }
        if (capture && !capture(ctx, input, texts)) return false;
    }
    auto model = get_model(config);
    if (!model) return false;
    std::vector<size_t> order;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.stats.calls;
        auto it = s.cache.find(key);
        if (it != s.cache.end()) order = it->second;
    }
    if (order.empty()) {
        try {
            auto t0 = std::chrono::steady_clock::now();
            std::vector<float> p = model->score(ctx, pinyin, texts);
            if (p.size() != texts.size()) return false;
            std::vector<double> mixed(p.size());
            for (size_t i = 0; i < p.size(); ++i)
                mixed[i] = std::log((std::max)(p[i], 1e-9f)) +
                           lc.rank_prior_weight * rank_prior(kRankPriorZh, i);
            order.resize(p.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return mixed[a] > mixed[b]; });
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(s.mu);
            s.stats.last_ms = ms;
            if (s.cache.size() > 4096) s.cache.clear();
            s.cache[key] = order;
        } catch (const std::exception& e) {
            laya_log(L"inference failed: " + laya::utf8_to_wide(e.what()));
            return false;
        }
    }

    // 3. The model's pick moves to the group's first slot (marked as recommended); every other
    //    entry, partial-input ones included, keeps its order behind it.
    const size_t top = order.front();
    if (top != 0) {
        CandidateEntry pick = std::move(result.entries[idx[top]]);
        result.entries.erase(result.entries.begin() + static_cast<std::ptrdiff_t>(idx[top]));
        result.entries.insert(result.entries.begin() + static_cast<std::ptrdiff_t>(idx[0]),
                              std::move(pick));
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.stats.reordered;
    }
    result.entries[idx[0]].candidate.recommended = true;
    return top != 0;
}

bool LayaRerank::apply_english(const Config& config, const std::string& context,
                               const std::string& typed, std::vector<EnglishWord>& words) {
    const auto& lc = config.laya;
    if (!lc.enable || !lc.english || words.size() < 2 || typed.empty()) return false;
    auto model = get_model(config);
    if (!model) return false;

    const std::string ctx =
        laya::utf8_tail(context, static_cast<size_t>((std::max)(0, lc.english_context_chars)));
    std::vector<std::string> texts;
    std::string key = std::string("en") + '\x1f' + ctx + '\x1f' + typed + '\x1f' +
                      std::to_string(lc.english_rank_prior_weight);
    for (const auto& w : words) {
        texts.push_back(w.text);
        key += '\x1f' + w.text;
        if (w.cost != 0.0f) key += '\x1e' + std::to_string(w.cost);  // learned bonuses change it
    }

    State& s = state();
    std::vector<size_t> order;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.stats.calls;
        auto it = s.cache.find(key);
        if (it != s.cache.end()) order = it->second;
    }
    if (order.empty()) {
        try {
            auto t0 = std::chrono::steady_clock::now();
            std::vector<float> p = model->score(ctx, typed, texts, laya::Task::kEnglish);
            if (p.size() != texts.size()) return false;
            // Frequency prior: score = 100 * Zipf, so P_freq is proportional to 10^(score/100).
            // Normalizing it over the candidates only shifts every log P_freq by the same amount,
            // which does not change the order.
            // Spelling corrections: the model was trained on completions of what was typed, so a
            // correction pays for its typing cost (english_correction_weight * log10 per unit).
            std::vector<double> mixed(p.size());
            for (size_t i = 0; i < p.size(); ++i)
                mixed[i] = std::log((std::max)(p[i], 1e-9f)) +
                           lc.english_rank_prior_weight * rank_prior(kRankPriorEn, i) +
                           lc.english_freq_weight * (words[i].score / 100.0) * std::log(10.0) -
                           lc.english_correction_weight * words[i].cost * std::log(10.0);
            order.resize(p.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return mixed[a] > mixed[b]; });
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(s.mu);
            s.stats.last_ms = ms;
            if (s.cache.size() > 4096) s.cache.clear();
            s.cache[key] = order;
        } catch (const std::exception& e) {
            laya_log(L"inference failed: " + laya::utf8_to_wide(e.what()));
            return false;
        }
    }

    // The model's pick goes first; the others keep the dictionary (frequency) order.
    const size_t top = order.front();
    if (top != 0) {
        std::rotate(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(top),
                    words.begin() + static_cast<std::ptrdiff_t>(top) + 1);
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.stats.reordered;
    }
    return true;
}

}  // namespace cxxime
