// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// list_translator_gpus(): the graphics cards Vulkan offers, as llama-server (Vulkan build) sees
// them. vulkan-1.dll is the system's Vulkan loader; it is loaded only while listing. The few
// Vulkan types used here are declared below (no Vulkan SDK headers): their layout is part of
// the Vulkan 1.0 ABI.

#include <cxxime/gpu_adapters.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cxxime {

namespace {

using VkInstance = struct VkInstance_T*;
using VkPhysicalDevice = struct VkPhysicalDevice_T*;
using VkResult = std::int32_t;
constexpr VkResult kVkSuccess = 0;
constexpr std::int32_t kStructureTypeApplicationInfo = 0;
constexpr std::int32_t kStructureTypeInstanceCreateInfo = 1;
constexpr std::uint32_t kApiVersion10 = 1u << 22;
constexpr std::uint32_t kDeviceTypeDiscreteGpu = 2;
constexpr std::uint32_t kMemoryHeapDeviceLocal = 1;

struct VkApplicationInfo {
    std::int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    std::uint32_t applicationVersion;
    const char* pEngineName;
    std::uint32_t engineVersion;
    std::uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    std::int32_t sType;
    const void* pNext;
    std::uint32_t flags;
    const VkApplicationInfo* pApplicationInfo;
    std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

// VkPhysicalDeviceProperties starts with these fields; the rest (limits, sparse properties) is
// not read, so it is received into a larger buffer.
struct VkPhysicalDevicePropertiesHead {
    std::uint32_t apiVersion;
    std::uint32_t driverVersion;
    std::uint32_t vendorID;
    std::uint32_t deviceID;
    std::uint32_t deviceType;
    char deviceName[256];
};
constexpr size_t kPropertiesBufferSize = 4096;  // sizeof(VkPhysicalDeviceProperties) is 824

struct VkMemoryType {
    std::uint32_t propertyFlags;
    std::uint32_t heapIndex;
};
struct VkMemoryHeap {
    std::uint64_t size;
    std::uint32_t flags;
};
struct VkPhysicalDeviceMemoryProperties {
    std::uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    std::uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520, "Vulkan 1.0 layout");

using CreateInstance = VkResult(__stdcall*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using DestroyInstance = void(__stdcall*)(VkInstance, const void*);
using EnumeratePhysicalDevices = VkResult(__stdcall*)(VkInstance, std::uint32_t*, VkPhysicalDevice*);
using GetPhysicalDeviceProperties = void(__stdcall*)(VkPhysicalDevice, void*);
using GetPhysicalDeviceMemoryProperties = void(__stdcall*)(VkPhysicalDevice,
                                                           VkPhysicalDeviceMemoryProperties*);

}  // namespace

std::vector<GpuAdapter> list_translator_gpus(std::uint64_t min_memory_mb) {
    std::vector<GpuAdapter> gpus;
    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) return gpus;
    auto create = reinterpret_cast<CreateInstance>(GetProcAddress(loader, "vkCreateInstance"));
    auto destroy = reinterpret_cast<DestroyInstance>(GetProcAddress(loader, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<EnumeratePhysicalDevices>(
        GetProcAddress(loader, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<GetPhysicalDeviceProperties>(
        GetProcAddress(loader, "vkGetPhysicalDeviceProperties"));
    auto memory = reinterpret_cast<GetPhysicalDeviceMemoryProperties>(
        GetProcAddress(loader, "vkGetPhysicalDeviceMemoryProperties"));
    VkInstance instance = nullptr;
    if (create && destroy && enumerate && properties && memory) {
        VkApplicationInfo app = {kStructureTypeApplicationInfo, nullptr, "zhiyu", 1, nullptr, 0,
                                 kApiVersion10};
        VkInstanceCreateInfo info = {kStructureTypeInstanceCreateInfo, nullptr, 0, &app, 0, nullptr,
                                     0, nullptr};
        if (create(&info, nullptr, &instance) != kVkSuccess) instance = nullptr;
    }
    if (instance) {
        std::uint32_t count = 0;
        std::vector<VkPhysicalDevice> devices;
        if (enumerate(instance, &count, nullptr) == kVkSuccess && count > 0) {
            devices.resize(count);
            if (enumerate(instance, &count, devices.data()) < kVkSuccess) count = 0;
            devices.resize(count);
        }
        alignas(8) unsigned char buffer[kPropertiesBufferSize] = {};
        std::vector<std::string> names;  // every device so far, as llama-server counts them
        for (std::uint32_t i = 0; i < devices.size(); ++i) {
            std::memset(buffer, 0, sizeof(buffer));
            properties(devices[i], buffer);
            VkPhysicalDevicePropertiesHead head;
            std::memcpy(&head, buffer, sizeof(head));
            head.deviceName[sizeof(head.deviceName) - 1] = '\0';
            const std::string name = head.deviceName;
            // The same name again (two cards of one model) is "<name> #2", as llama-server
            // lists them in this order too; integrated graphics are not offered.
            names.push_back(name);
            const auto same_name = std::count(names.begin(), names.end(), name);
            if (head.deviceType != kDeviceTypeDiscreteGpu) continue;
            VkPhysicalDeviceMemoryProperties heaps = {};
            memory(devices[i], &heaps);
            std::uint64_t local = 0;
            for (std::uint32_t h = 0; h < heaps.memoryHeapCount && h < 16; ++h) {
                if (heaps.memoryHeaps[h].flags & kMemoryHeapDeviceLocal) {
                    local = (std::max)(local, heaps.memoryHeaps[h].size);
                }
            }
            if (local < (min_memory_mb << 20) || name.empty()) continue;
            GpuAdapter gpu;
            gpu.name = name;
            gpu.key = same_name > 1 ? name + " #" + std::to_string(same_name) : name;
            gpu.index = i;
            gpu.memory = local;
            gpu.driver_version = head.driverVersion;
            gpus.push_back(gpu);
        }
        destroy(instance, nullptr);
    }
    FreeLibrary(loader);
    return gpus;
}

}  // namespace cxxime
