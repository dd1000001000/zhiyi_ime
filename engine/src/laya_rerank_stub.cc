// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// LayaRerank for the x86 build, which only produces the in-process modules for 32-bit
// applications. The model runs in the 64-bit server, so candidates keep their order here.

#include <cxxime/laya_rerank.h>

namespace cxxime {

LayaRerank& LayaRerank::instance() {
    static LayaRerank rerank;
    return rerank;
}

bool LayaRerank::apply(const Config&, const std::string&, const std::string&, TranslationResult&) {
    return false;
}

bool LayaRerank::apply_english(const Config&, const std::string&, const std::string&,
                               std::vector<EnglishWord>&) {
    return false;
}

void LayaRerank::preload(const Config&) {}

LayaGpuTest LayaRerank::test_gpu(const Config&, const std::string&) {
    LayaGpuTest result;
    result.error = "no model in this build";
    return result;
}

std::string LayaRerank::model_id(const Config&) const {
    return {};
}

LayaRerankStats LayaRerank::stats() const {
    return {};
}

}  // namespace cxxime
