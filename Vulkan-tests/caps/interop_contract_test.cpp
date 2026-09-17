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

}

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
        {"drm_modifiers", d.drm_modifiers, false},
        {"dma_buf_fd", d.dma_buf_fd, false},
        {"maintenance4", d.maintenance4, false},
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