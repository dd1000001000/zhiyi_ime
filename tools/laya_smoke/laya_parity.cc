// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Scores bench samples with the C++ reranker (the code the IME runs) and reports top-1 accuracy;
// with an output path, also writes the probabilities so they can be compared with Python
// (bench/eval_int8.py uses the same model files).
//
//   laya_parity <model_dir> <samples.jsonl> [limit] [probs_out.jsonl]
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <json.hpp>

#include "laya/reranker.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        std::printf("usage: laya_parity <model_dir> <samples.jsonl> [limit] [probs_out.jsonl]\n");
        return 2;
    }
    laya::RerankerOptions opt;
    opt.model_dir = argv[1];
    const int limit = argc > 3 ? std::atoi(argv[3]) : 0;
    laya::Reranker model(opt);

    std::ifstream in(argv[2], std::ios::binary);
    std::ofstream out;
    if (argc > 4) out.open(argv[4], std::ios::binary);
    std::string line;
    int n = 0, ok = 0;
    std::vector<double> ms;
    while (std::getline(in, line) && (limit <= 0 || n < limit)) {
        if (line.empty()) continue;
        const auto it = nlohmann::json::parse(line);
        const bool english = it.value("lang", "") == "en";
        const std::string input = english ? it["typed"].get<std::string>() : it["pinyin"].get<std::string>();
        const auto cands = it["candidates"].get<std::vector<std::string>>();
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<float> p = model.score(it["context"].get<std::string>(), input, cands,
                                                 english ? laya::Task::kEnglish : laya::Task::kPinyin);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        const int pred = static_cast<int>(std::max_element(p.begin(), p.end()) - p.begin());
        ok += pred == it["gold_rank"].get<int>();
        if (out) out << nlohmann::json{{"i", n}, {"probs", p}}.dump() << "\n";
        ++n;
    }
    std::sort(ms.begin(), ms.end());
    std::printf("%d samples, top-1 %.4f, p50 %.1f ms\n", n, n ? static_cast<double>(ok) / n : 0.0,
                ms.empty() ? 0.0 : ms[ms.size() / 2]);
    return 0;
}
