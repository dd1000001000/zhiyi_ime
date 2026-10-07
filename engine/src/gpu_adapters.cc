// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/gpu_adapters.h>

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <intrin.h>

#include <cstring>
#include <tuple>
#include <vector>

namespace cxxime {

namespace {

std::string to_utf8(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string trim(std::string text) {
    const size_t begin = text.find_first_not_of(' ');
    const size_t end = text.find_last_not_of(' ');
    return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

using PciAddress = std::tuple<UINT, UINT, UINT>;  // bus, device, function
constexpr UINT kNoBus = 0xFFFFFFFFu;

// The adapter's PCI location; bus kNoBus when it has none (a view another driver adds).
PciAddress pci_address(const LUID& luid) {
    PciAddress none{kNoBus, 0, 0};
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gdi) return none;
    auto open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    auto query = reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    auto close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return none;
    D3DKMT_OPENADAPTERFROMLUID adapter = {};
    adapter.AdapterLuid = luid;
    if (open(&adapter) != 0) return none;
    D3DKMT_ADAPTERADDRESS address = {};
    D3DKMT_QUERYADAPTERINFO info = {};
    info.hAdapter = adapter.hAdapter;
    info.Type = KMTQAITYPE_ADAPTERADDRESS;
    info.pPrivateDriverData = &address;
    info.PrivateDriverDataSize = sizeof(address);
    const bool known = query(&info) == 0;
    D3DKMT_CLOSEADAPTER done = {};
    done.hAdapter = adapter.hAdapter;
    close(&done);
    return known ? PciAddress{address.BusNumber, address.DeviceNumber, address.FunctionNumber} : none;
}

}  // namespace

std::vector<GpuAdapter> list_gpu_adapters() {
    std::vector<GpuAdapter> adapters;
    std::vector<PciAddress> addresses;
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = dxgi ? reinterpret_cast<CreateFactory>(
                                     GetProcAddress(dxgi, "CreateDXGIFactory1"))
                               : nullptr;
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                                     GetProcAddress(d3d12, "D3D12CreateDevice"))
                               : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create_factory && create_device &&
        SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc = {};
            // S_FALSE: a device could be created (the device itself is not requested).
            const bool usable =
                SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory >= kMinGpuMemory &&
                create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr) ==
                    S_FALSE;
            if (usable) {
                const std::string name = to_utf8(desc.Description);
                const PciAddress address = pci_address(desc.AdapterLuid);
                int same_name = 0;
                bool listed = false;
                for (size_t k = 0; k < adapters.size(); ++k) {
                    if (adapters[k].name != name) continue;
                    ++same_name;
                    // The same card again: same slot, or (no slot to compare) the same name.
                    listed = listed || std::get<0>(address) == kNoBus ||
                             std::get<0>(addresses[k]) == kNoBus || addresses[k] == address;
                }
                if (!listed) {
                    LARGE_INTEGER umd = {};
                    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
                    GpuAdapter item;
                    item.name = name;
                    item.key = same_name == 0 ? name : name + " #" + std::to_string(same_name + 1);
                    item.index = i;
                    item.memory = desc.DedicatedVideoMemory;
                    item.driver_version = static_cast<std::uint64_t>(umd.QuadPart);
                    adapters.push_back(std::move(item));
                    addresses.push_back(address);
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
    // Kept loaded: ONNX Runtime's DirectML provider uses both when the model moves to a card.
    return adapters;
}

const GpuAdapter* find_gpu_adapter(const std::vector<GpuAdapter>& adapters, const std::string& key) {
    for (const GpuAdapter& adapter : adapters) {
        if (adapter.key == key) return &adapter;
    }
    return nullptr;
}

CpuInfo cpu_info() {
    CpuInfo info;
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, static_cast<int>(0x80000000));
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        for (int leaf = 0; leaf < 3; ++leaf) {
            __cpuid(regs, static_cast<int>(0x80000002u + leaf));
            memcpy(brand + leaf * 16, regs, sizeof(regs));
        }
    }
    info.name = trim(brand);
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorPackage, nullptr, &bytes);
    if (bytes > 0) {
        std::vector<char> buffer(bytes);
        auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorPackage, first, &bytes)) {
            unsigned packages = 0;
            for (DWORD offset = 0; offset < bytes;) {
                auto* item = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
                ++packages;
                offset += item->Size;
            }
            if (packages > 0) info.packages = packages;
        }
    }
    return info;
}

}  // namespace cxxime
