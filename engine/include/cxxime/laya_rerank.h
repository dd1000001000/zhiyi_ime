// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Laya context reranking: reorders the first page of pinyin candidates with the Laya decision
// model (ONNX, CPU), using the text committed so far as context.
//
// The model is loaded once per process on a background thread and shared by every session;
// until it is ready, or if anything fails, candidates keep the translator's order.
#ifndef CXXIME_LAYA_RERANK_H_
#define CXXIME_LAYA_RERANK_H_

#include <functional>
#include <string>
#include <vector>

namespace cxxime {

struct Config;
struct TranslationResult;
struct EnglishWord;

struct LayaRerankStats {
    bool model_ready = false;
    bool model_failed = false;
    long long calls = 0;
    long long reordered = 0;
    double last_ms = 0.0;
};

class LayaRerank {
public:
    static LayaRerank& instance();

    // Looks at the first 2 * config.page_size entries. When at least
    // `config.laya.min_candidates` of them cover the whole input (any length: 显示 / 西安市),
    // moves the model's pick among those to the first of their slots and marks it
    // `recommended`; every other entry keeps its order, and candidates for part of the input
    // (飘 for "piaol") keep their places.
    // `input` is the raw pinyin the translator was queried with. Returns true if the order
    // changed.
    bool apply(const Config& config, const std::string& context, const std::string& input,
               TranslationResult& result);

    // English word mode: moves the model's pick among the completions of `typed` (dictionary
    // order: exact match first, then by frequency) to the front; the pick maximizes
    // log P_laya + config.laya.english_freq_weight * log P_freq. The others keep their order.
    // Returns true when the model ran (words.front() is then its pick).
    bool apply_english(const Config& config, const std::string& context, const std::string& typed,
                       std::vector<EnglishWord>& words);

    // Tools: called with what the model would compare (context, input, candidate texts in
    // order) before it runs; returning false skips the model (the order stays). Not for the IME.
    using CaptureFn = std::function<bool(const std::string& context, const std::string& input,
                                         const std::vector<std::string>& texts)>;
    void set_capture(CaptureFn capture);

    // Starts loading the model in the background (no-op when already loaded or loading).
    void preload(const Config& config);

    LayaRerankStats stats() const;

private:
    LayaRerank() = default;
};

}  // namespace cxxime

#endif  // CXXIME_LAYA_RERANK_H_
