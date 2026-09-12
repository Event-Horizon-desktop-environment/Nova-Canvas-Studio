// interop_contract_test — the extension floor the backend's interop design sits on.
//
// Phase 0 gate for the architecture chosen in docs/vulkan.md: a SHARED device
// (one VkInstance/device, all queues from it) whose image/semaphore/memory
// handles move between queues and into FFmpeg via the Vulkan hwdevice context.
// That design hard-depends on a specific extension set being present. This test
// pins THAT contract on the machine: if any member is missing, the design as
// documented cannot be built here and the owning phase must be re-planned —
// failing here at Phase 0 is far cheaper than failing mid-P-C/P-D.
//
// Extensions asserted:
//   VK_KHR_timeline_semaphore      — cross-queue A/V/composite ordering
//   VK_KHR_synchronization2        — modern transition/signal barriers
//   VK_KHR_external_memory_fd      — export memory to FFmpeg's vulkan ctx
//   VK_KHR_external_semaphore_fd   — export semaphores to FFmpeg's vulkan ctx
//   VK_EXT_host_query_reset        — low-latency host->device query resets
//   VK_KHR_push_descriptor         — cheap descriptor updates per composite
//   VK_EXT_image_drm_format_modifier — NV12/dmabuf tiling paths (if DRM target)
//   VK_EXT_memory_budget           — the vram_leak_vulkan sampling seam
//
// PASS (0)  — primaries have the full floor (core-promoted or named).
// FAIL (1)  — a required named extension is missing from the device.
// SKIP (2)  — no Vulkan implementation at all.

#include "vk_probe.hpp"

using namespace canvas::vktest;

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    const ProbeResult r = run_probe();
    if (!r.ok || r.devices.empty()) {
        std::printf("SKIP  interop_contract: no Vulkan implementation (%s)\n",
                    r.ok ? "no devices" : r.error.c_str());
        return 2;
    }

    const DeviceInfo& d = r.devices.front();
    std::printf("interop_contract: %s (api=%u.%u.%u)\n",
                d.name.c_str(),
                VK_API_VERSION_MAJOR(d.api_version),
                VK_API_VERSION_MINOR(d.api_version),
                VK_API_VERSION_PATCH(d.api_version));

    struct Floor { const char* what; bool present; bool mandatory; };
    const Floor floor[] = {
        {"timeline_semaphores", d.timeline_semaphores, true},
        {"sync2", d.sync2, true},
        {"external_memory_fd", d.external_memory_fd, true},
        {"external_semaphore_fd", d.external_semaphore_fd, true},
        {"host_query_reset", d.host_query_reset, true},
        {"push_descriptor", d.push_descriptor, true},
        {"memory_budget", d.memory_budget, true},
        {"drm_modifiers", d.drm_modifiers, false},   // DRM-target only
        {"dma_buf_fd", d.dma_buf_fd, false},         // Linux-target only
        {"maintenance4", d.maintenance4, false},     // nice-to-have (soft)
    };

    for (const Floor& f : floor) {
        if (f.mandatory) {
            check(f.present, f.what);
        } else {
            std::printf("%s  %s (soft/optional)\n",
                        f.present ? "PASS" : "note", f.what);
        }
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}