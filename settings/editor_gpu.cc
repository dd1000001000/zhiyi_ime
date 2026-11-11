// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The graphics cards and the processor, asked once per run, for the pages that offer a card: voice
// input (voice.device) and the translator (learning.translator_device). The word choice model ran on
// a card too (laya.device, a "Device" row on the General page with a speed test) until 1.6.5: it
// runs on the CPU only now.

#include "editor_app.h"

#include <map>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {

const std::vector<GpuAdapter>& cached_gpu_adapters(std::uint64_t min_memory_mb) {
    static std::map<std::uint64_t, std::vector<GpuAdapter>> lists;
    const auto found = lists.find(min_memory_mb);
    return found != lists.end() ? found->second
                                : lists.emplace(min_memory_mb, list_gpu_adapters(min_memory_mb)).first->second;
}

const std::vector<GpuAdapter>& cached_translator_gpus(std::uint64_t min_memory_mb) {
    static std::map<std::uint64_t, std::vector<GpuAdapter>> lists;
    const auto found = lists.find(min_memory_mb);
    return found != lists.end() ? found->second
                                : lists.emplace(min_memory_mb, list_translator_gpus(min_memory_mb)).first->second;
}

const CpuInfo& cached_cpu_info() {
    static const CpuInfo info = cpu_info();
    return info;
}

}  // namespace settings
}  // namespace cxxime
