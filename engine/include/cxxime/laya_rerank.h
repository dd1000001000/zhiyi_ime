// Copyright (c) 2026 Laya IME Contributors. Apache License 2.0.
//
// Laya context reranking: reorders the first page of pinyin candidates with the Laya decision
// model (ONNX, CPU), using the text committed so far as context.
//
// The model is loaded once per process on a background thread and shared by every session;
// until it is ready, or if anything fails, candidates keep the translator's order.
#ifndef CXXIME_LAYA_RERANK_H_
#define CXXIME_LAYA_RERANK_H_

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

    // When the first page holds at least `config.laya.min_candidates` same-length candidates
    // covering the same input span as the first candidate, moves the model's pick among them to
    // the first slot (the others keep their order) and marks it `recommended`.
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

    // Starts loading the model in the background (no-op when already loaded or loading).
    void preload(const Config& config);

    LayaRerankStats stats() const;

private:
    LayaRerank() = default;
};

}  // namespace cxxime

#endif  // CXXIME_LAYA_RERANK_H_
