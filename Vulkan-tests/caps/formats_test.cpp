// formats_test — image format support matrix for the composite path (P-D).
//
// Phase 0 reading that pins which VkFormat combos the machine supports for the
// way the backend composites (docs/vulkan.md, Phase P-D): the decoder hands us
// an NV12 image on a video-decode queue, the composite kernel samples it as a
// texture on a graphics/compute queue and writes RGBA8/SRGB output images, and
// the encode path reads those outputs. Every one of those is a VkFormat +
// image-usage + tiling combination that a driver may or may not support — NV12
// sampled on compute is driver-specific.
//
// This test enumerates the exact combos the plan assumes and reports PASS/FAIL
// per combo. It does not require a VkDevice: format support is a physical-
// device query. A machine that cannot sample NV12 with a STORAGE/SAMPLED image
// (composite target) fails loudly NOW instead of in P-D's face at build time.
//
// PASS (0)  — every combo the plan's phase needs on this machine is supported.
// FAIL (1)  — a REQUIRED combo is missing (the phase that needs it cannot ship
//             on this machine).
// SKIP (2)  — no Vulkan implementation at all.

#include "vk_probe.hpp"
#include "vk_session.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

using namespace canvas::vktest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Combo {
    const char* name;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    bool required;  // P-D cannot ship without it
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

}  // namespace

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

    // The composite plan's format combos. `required` marks combos the plan
    // depends on unconditionally for P-D to ship on this machine type.
    const Combo combos[] = {
        // Decoder -> composite: sample NV12 (2-plane) as a texture.
        {"nv12-sample-optimal", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         true},
        {"nv12-sample-linear", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         false},
        // Composite target: RGBA8 color attachment (sample + render).
        {"rgba8-color-optimal", VK_FORMAT_R8G8B8A8_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         true},
        // Composite target: SRGB (encoded output of a grade carries an sRGB
        // transfer).
        {"rgba8s-sampled-optimal", VK_FORMAT_R8G8B8A8_SRGB,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         false},
        // Compute direct-write target (if the composite routes through a
        // compute pass instead of a render pass).
        {"nv12-storage-optimal", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, false},
        // CPU-export / parity-readback target.
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