// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Where the Laya model can run (laya.device): the CPU, or a graphics card through DirectML.
// Cards offered: hardware adapters that create a Direct3D 12 device (feature level 11_0) and
// have at least kMinGpuMemory of their own memory. Integrated graphics share system memory and
// were slower than the CPU in our measurements, so they are left out. A card is listed once
// even when Windows enumerates it more than once (a virtual display driver adds a view of it
// without a PCI address); two cards of the same model are both listed. dxgi.dll, d3d12.dll
// and the D3DKMT functions are looked up on demand: processes that never ask (the text
// service in every application) do not load them.
#ifndef CXXIME_GPU_ADAPTERS_H_
#define CXXIME_GPU_ADAPTERS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace cxxime {

constexpr std::uint64_t kMinGpuMemory = 2ull << 30;  // 2 GB of dedicated video memory

struct GpuAdapter {
    std::string name;  // UTF-8 description ("NVIDIA GeForce RTX 5070 Ti")
    // The laya.device value: the name, or "<name> #2" for the second card of the same model
    // (in enumeration order, which follows the PCI slots).
    std::string key;
    unsigned index = 0;               // IDXGIFactory1::EnumAdapters1 index (DirectML device_id)
    std::uint64_t memory = 0;         // dedicated video memory, bytes
    std::uint64_t driver_version = 0;  // user-mode driver version (a test result is per driver)
};

std::vector<GpuAdapter> list_gpu_adapters();

// The adapter whose key is `key`, or nullptr.
const GpuAdapter* find_gpu_adapter(const std::vector<GpuAdapter>& adapters, const std::string& key);

struct CpuInfo {
    std::string name;       // UTF-8 brand string ("AMD Ryzen 9 9950X 16-Core Processor")
    unsigned packages = 1;  // processor sockets; the CPU option uses all of them
};

CpuInfo cpu_info();

}  // namespace cxxime

#endif  // CXXIME_GPU_ADAPTERS_H_
