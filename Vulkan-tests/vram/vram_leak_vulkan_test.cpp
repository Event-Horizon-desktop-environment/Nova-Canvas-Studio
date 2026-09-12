// vram_leak_vulkan_test — device-memory steady-state gate via VK_EXT_memory_budget.
//
// Phase 0 reading for core's vram_leak (the CUDA regression gate that pinned
// the av_frame_ref-onto-dirty-retain_hw_ leak filling 16 GB in ~90 s). When the
// Vulkan decode path lands (Phase P-C), the SAME leak class is exercised here:
// repeated decode sessions must drain no steady-state memory per the budget.
//
// Phase 0 scope: this reading exists NOW, wiring the memory_budget sampling
// seam and the drain law headlessly, so P-C's decode drops in behind it with a
// target already in place. It performs real allocation churn against the device
// heap and asserts the law: steady-state free memory may move, it may not
// monotonically drain past a fixed budget.
//
// PASS (0)  — allocation churn against a device heap leaves steady-state free
//             memory within budget after warm-up.
// FAIL (1)  — drain tracked past the budget (same failure class vram_leak pins).
// SKIP (2)  — no device offers VK_EXT_memory_budget (lowest common denominator
//             on others' machines; the owning phase is P-C).

#include "vk_probe.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace canvas::vktest;

namespace {

constexpr std::uint64_t kWarmBytes = 256ull * 1024 * 1024;      // working set before baseline
constexpr std::uint64_t kChurnBytes = 32ull * 1024 * 1024;      // per churn cycle
constexpr int kChurnRounds = 48;
constexpr float kBudgetSlack = 0.10f;  // free-heap may swing 10% around baseline

}  // namespace

int main() {
    const ProbeResult r = run_probe();
    if (!r.ok || r.devices.empty()) {
        std::printf("SKIP  vram_leak_vulkan: no Vulkan implementation (%s)\n",
                    r.ok ? "no devices" : r.error.c_str());
        return 2;
    }

    // Pick the primary device that offers the memory-budget extension.
    const DeviceInfo* chosen = nullptr;
    for (const auto& d : r.devices) {
        if (d.memory_budget) {
            chosen = &d;
            break;
        }
    }
    if (chosen == nullptr) {
        std::printf("SKIP  vram_leak_vulkan: no device offers VK_EXT_memory_budget "
                    "(owning phase P-C lands the decode leg)\n");
        return 2;
    }
    std::printf("info: vram_leak_vulkan using device %s\n", chosen->name.c_str());

    // Instance (VK_KHR_get_physical_device_properties2 is core 1.1+, fine at 1.3).
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "canvas-vram-leak-vulkan";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        std::printf("SKIP  vram_leak_vulkan: cannot create instance\n");
        return 2;
    }

    // Find the sibling VkPhysicalDevice (probe only holds names).
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pd(n);
    vkEnumeratePhysicalDevices(inst, &n, pd.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (VkPhysicalDevice p : pd) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (chosen->name == props.deviceName) {
            phys = p;
            break;
        }
    }
    if (phys == VK_NULL_HANDLE) {
        vkDestroyInstance(inst, nullptr);
        std::printf("SKIP  vram_leak_vulkan: probe device not re-enumerable\n");
        return 2;
    }

    // A compute queue family suffices for allocation churn.
    std::uint32_t qf = UINT32_MAX;
    for (const auto& q : chosen->queue_families) {
        if (q.compute) {
            qf = q.index;
            break;
        }
    }
    if (qf == UINT32_MAX) {
        vkDestroyInstance(inst, nullptr);
        std::printf("SKIP  vram_leak_vulkan: no compute queue family\n");
        return 2;
    }
    std::printf("info: vram_leak_vulkan queue_family=%u graphics+qf\n", qf);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_ext = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &dev_ext;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
        vkDestroyInstance(inst, nullptr);
        std::printf("SKIP  vram_leak_vulkan: device creation with memory_budget failed\n");
        return 2;
    }

    // Willing allocations: any heap counted by the budget, any memory type
    // reachable from the chosen queue (device-local preferred).
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    const auto free_bytes = [&](const VkPhysicalDevice& p, std::uint64_t* free_hi) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(p, &props2);
        // "Free" = budget (max allocatable) minus usage on the biggest heap.
        std::uint64_t budget_total = 0, usage_total = 0;
        for (std::uint32_t i = 0; i < props2.memoryProperties.memoryHeapCount; ++i) {
            budget_total += budget.heapBudget[i];
            usage_total += budget.heapUsage[i];
        }
        *free_hi = budget_total > usage_total ? budget_total - usage_total : 0;
        return true;
    };

    // A single alloc + free against each memory type we may use.
    const auto churn_allocate = [&](std::uint64_t bytes, VkDeviceMemory* out) {
        for (std::uint32_t type = 0; type < mp.memoryTypeCount; ++type) {
            if (!(mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                continue;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = bytes;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(dev, &ai, nullptr, out) == VK_SUCCESS) return true;
        }
        return false;
    };

    // Warm-up: build a working set, free it, let the allocator settle.
    std::vector<VkDeviceMemory> warm;
    while (true) {
        std::uint64_t held = 0;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (!churn_allocate(kChurnBytes, &m)) break;
        warm.push_back(m);
        held += kChurnBytes;
        if (held >= kWarmBytes) break;
    }
    for (VkDeviceMemory m : warm) vkFreeMemory(dev, m, nullptr);
    vkDeviceWaitIdle(dev);

    std::uint64_t free_b0 = 0;
    free_bytes(phys, &free_b0);
    const std::uint64_t baseline = free_b0;

    // Churn rounds allocating + freeing a fixed slice; track the min free.
    std::uint64_t min_free = baseline;
    std::uint64_t worst_round = 0;
    for (int r = 0; r < kChurnRounds; ++r) {
        std::vector<VkDeviceMemory> batch;
        bool ok = true;
        for (int k = 0; k < 2 && ok; ++k) {
            VkDeviceMemory m = VK_NULL_HANDLE;
            if (churn_allocate(kChurnBytes, &m)) batch.push_back(m);
            else ok = false;
        }
        for (VkDeviceMemory m : batch) vkFreeMemory(dev, m, nullptr);
        if ((r % 8) == 0) {
            std::uint64_t f = 0;
            free_bytes(phys, &f);
            if (f < min_free) {
                min_free = f;
                worst_round = r;
            }
        }
    }
    vkDeviceWaitIdle(dev);

    std::uint64_t free_end = 0;
    free_bytes(phys, &free_end);

    const std::uint64_t drift = baseline > free_end ? baseline - free_end : 0;
    const std::uint64_t slack = (std::uint64_t)((double)baseline * kBudgetSlack);
    std::printf("info: vram_leak_vulkan baseline_free=%lluMB min_free=%lluMB (round %llu) "
                "end_free=%lluMB drift=%lluMB slack=%lluMB\n",
                (unsigned long long)(baseline >> 20), (unsigned long long)(min_free >> 20),
                (unsigned long long)worst_round, (unsigned long long)(free_end >> 20),
                (unsigned long long)(drift >> 20), (unsigned long long)(slack >> 20));

    const bool within_budget = drift <= slack;
    std::printf("%s  vram_leak_vulkan: steady-state free memory stays within budget after churn\n",
                within_budget ? "PASS" : "FAIL");

    if (within_budget) {
        std::printf("ALL TESTS PASSED\n");
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        return 0;
    }
    std::printf("1 FAILURE(S)\n");
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 1;
}