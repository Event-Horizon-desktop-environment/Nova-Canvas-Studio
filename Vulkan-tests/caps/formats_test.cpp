#include "vk_probe.hpp"
#include "vk_session.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

using namespace canvas::vktest;

namespace {

int g_failures = 0;

struct Combo {
    const char* name;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    bool required;
};

bool supports(VkPhysicalDevice phys, const Combo& c) {
    VkPhysicalDeviceImageFormatInfo2 fmt{};
    fmt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    fmt.format = c.format;
    fmt.type = VK_IMAGE_TYPE_2D;
    fmt.tiling = c.tiling;
    fmt.usage = c.usage;
    fmt.flags = 0;

    VkImageFormatProperties2 prop{};
    prop.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;

    return vkGetPhysicalDeviceImageFormatProperties2(phys, &fmt, &prop) == VK_SUCCESS;
}

}

int main() {
    const ProbeResult r = run_probe();
    if (!r.ok || r.devices.empty()) {
        std::printf("SKIP  formats: no Vulkan implementation (%s)\n",
                    r.ok ? "no devices" : r.error.c_str());
        return 2;
    }

    const Session s(r.devices.front().name);
    if (s.physical == VK_NULL_HANDLE) {
        std::printf("SKIP  formats: cannot enumerate physical device\n");
        return 2;
    }
    std::printf("formats: probing %s\n", s.device_name.c_str());

    const Combo combos[] = {
        {"nv12-sample-optimal", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         true},
        {"nv12-sample-linear", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         false},
        {"rgba8-color-optimal", VK_FORMAT_R8G8B8A8_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         true},
        {"rgba8s-sampled-optimal", VK_FORMAT_R8G8B8A8_SRGB,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         false},
        {"nv12-storage-optimal", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, false},
        {"rgba8-linear-readback", VK_FORMAT_R8G8B8A8_UNORM,
         VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, false},
    };

    for (const Combo& c : combos) {
        const bool ok = supports(s.physical, c);
        if (c.required && !ok) ++g_failures;
        std::printf("%s  formats: %-24s (required=%s)\n",
                    ok ? "PASS" : (c.required ? "FAIL" : "PASS-unrequired"),
                    c.name, c.required ? "yes" : "no");
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}