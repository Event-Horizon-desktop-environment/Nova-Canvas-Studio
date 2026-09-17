#include "vk_probe.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

using namespace canvas::vktest;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static void print_device(const DeviceInfo& d, int index) {
    std::printf("  device[%d] name=%s driver=%s\n", index,
                d.name.c_str(), d.driver_name.c_str());
    std::printf("    api=%u.%u.%u driver_ver=%u conformance=%s",
                VK_API_VERSION_MAJOR(d.api_version),
                VK_API_VERSION_MINOR(d.api_version),
                VK_API_VERSION_PATCH(d.api_version),
                d.driver_version,
                d.conformance_ok ? "yes" : "no");
    if (d.conformance_ok) {
        std::printf(" (%u.%u.%u.%u)", d.conformance_major, d.conformance_minor,
                    d.conformance_subminor, d.conformance_patch);
    }
    std::printf("\n");
    std::printf("    families=%zu  video_decode=%s  video_encode=%s\n",
                d.queue_families.size(),
                d.has_video_decode_family ? "yes" : "no",
                d.has_video_encode_family ? "yes" : "no");
    std::printf("    memory_budget=%s  drm_modifiers=%s  dma_buf_fd=%s  timeline_sem=%s\n",
                d.memory_budget ? "yes" : "no",
                d.drm_modifiers ? "yes" : "no",
                d.dma_buf_fd ? "yes" : "no",
                d.timeline_semaphores ? "yes" : "no");
}

int main() {
    const ProbeResult r = run_probe();

    if (!r.ok) {
        std::printf("SKIP  Vulkan implementation unavailable: %s\n", r.error.c_str());
        return 2;
    }

    std::printf("vk_probe: instance_api=%u.%u.%u devices=%zu\n",
                VK_API_VERSION_MAJOR(r.instance_api),
                VK_API_VERSION_MINOR(r.instance_api),
                VK_API_VERSION_PATCH(r.instance_api),
                r.devices.size());

    check(!r.devices.empty(), "vk_probe: at least one physical device");
    if (r.devices.empty()) {
        std::printf("SKIP  probe ran but enumerated no devices\n");
        return 2;
    }

    for (std::size_t i = 0; i < r.devices.size(); ++i)
        print_device(r.devices[i], static_cast<int>(i));

    bool qf_sane = true;
    for (const auto& d : r.devices) {
        for (const auto& q : d.queue_families) {
            const bool video_queue = (q.flags & (VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                 VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) != 0;
            if ((q.video_decode || q.video_encode) && !video_queue) qf_sane = false;
        }
    }
    check(qf_sane, "vk_probe: queue-family video bits coherent with queue flags");

    bool any_drm = false, any_budget = false, any_timeline = false;
    for (const auto& d : r.devices) {
        any_drm |= d.drm_modifiers && d.dma_buf_fd;
        any_budget |= d.memory_budget;
        any_timeline |= d.timeline_semaphores;
    }
    std::printf("info: interop gates -> memory_budget=%s drm+dma_buf=%s timeline_sem=%s\n",
                any_budget ? "any" : "none",
                any_drm ? "any" : "none",
                any_timeline ? "any" : "none");

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}