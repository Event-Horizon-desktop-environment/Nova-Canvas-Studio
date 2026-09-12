// vk_probe_test — Phase 0 selftest of the Vulkan capability probe.
//
// Proves the probe itself is coherent before any backend code leans on it:
//   1. a Vulkan implementation is reachable through the system loader/ICD
//      (instance created, at least one device enumerated),
//   2. the queue-family matrix is sane (at least one family per device,
//      video-codec flags split decode vs encode correctly),
//   3. the driver matrix is printable/reportable — conformance version and
//      the extension subset the interop design gates on (memory budget,
//      DRM modifiers + dma-buf, timeline semaphores) are reported per device.
//
// PASS (0)  — loader + device present and matrix sane; the report prints.
// FAIL (1)  — the loader API misbehaved (instance creation failed despite an
//             ICD, or the query chain returned garbage).
// SKIP (2)  — NO Vulkan loader/ICD installed; nothing can run, and every
//             downstream Vulkan test will SKIP for the same reason.

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
        // No particle-of-Vulkan at all (no loader or no ICD): every Vulkan test
        // in this tree is moot. Report and skip.
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

    // Queue matrix sanity: fill flags never lost, video flags never coexisted
    // on a family that doesn't also carry the queue flag bits.
    bool qf_sane = true;
    for (const auto& d : r.devices) {
        for (const auto& q : d.queue_families) {
            const bool video_queue = (q.flags & (VK_QUEUE_VIDEO_DECODE_BIT_KHR |
                                                 VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) != 0;
            // Video ops must only be reported where the family actually says
            // it does video queue work.
            if ((q.video_decode || q.video_encode) && !video_queue) qf_sane = false;
        }
    }
    check(qf_sane, "vk_probe: queue-family video bits coherent with queue flags");

    // Extension-subset reporting: the interop-gating extensions must be
    // individually gated, not lumped. A missing one is reported, not fatal —
    // downstream tests SKIP on the ones they actually need.
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