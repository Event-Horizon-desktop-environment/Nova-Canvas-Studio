// export_sweep_vulkan_test — Vulkan encoder sweep coverage.
//
// Phase 0 reading for core's export_sweep (which covers every valid
// codec x container combo the Deliver panel can offer). The Vulkan encoders the
// backend will route its encode path through (Phases P-C/P-E) are also FFmpeg
// encoders, so they must come from the SAME codec list the Deliver panel and
// export_sweep use — this reading proves the exporter's own codec discovery
// reports the Vulkan encoders FFmpeg exposes, exactly the way export_sweep
// proves every other codec combo works.
//
// It does NOT run a full encode: the FFmpeg Vulkan encode path first needs the
// backend's device/queue plumbing (an owning P-E phase). What Phase 0 pins is
// the codec-discovery half of the law, here and now, against the encoder table.
//
// PASS (0)  — every Vulkan encoder FFmpeg registers is reported by the
//             exporter's codec list with hw=true / hw_device="vulkan".
// FAIL (1)  — a registered Vulkan encoder is missing from the exporter list
//             (would silently strand the Deliver panel on it later).
// SKIP (2)  — linked FFmpeg has no Vulkan encoder at all; owning phase P-E.

#include "canvas/core/export/exporter.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace canvas::core;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    const std::array<const char*, 5> kVulkanEncoders = {
        "h264_vulkan", "hevc_vulkan", "av1_vulkan",
        "ffv1_vulkan", "prores_ks_vulkan",
    };

    std::vector<std::string> registered;
    for (const char* name : kVulkanEncoders) {
        const AVCodec* c = avcodec_find_encoder_by_name(name);
        if (c != nullptr) registered.emplace_back(name);
    }
    if (registered.empty()) {
        std::printf("SKIP  export_sweep_vulkan: linked FFmpeg has no Vulkan video encoder "
                    "(owning phase P-E builds the encode path on it)\n");
        return 2;
    }

    for (const std::string& name : registered)
        std::printf("info: FFmpeg registers vulkan encoder %s\n", name.c_str());

    // The exporter's own codec list must see each registered Vulkan encoder.
    const std::vector<CodecInfo> all = list_video_codecs("vulkan");
    bool all_seen = true;
    for (const std::string& name : registered) {
        const auto it = std::find_if(all.begin(), all.end(),
                                     [&name](const CodecInfo& c) { return c.name == name; });
        const bool seen = it != all.end();
        const bool flagged = seen && it->hw && it->hw_device == "vulkan";
        std::printf("%s  export_sweep_vulkan: %s in exporter list (hw=vulkan)\n",
                    (seen && flagged) ? "PASS" : "FAIL", name.c_str());
        if (!(seen && flagged)) {
            all_seen = false;
            ++g_failures;
        }
    }

    // No stray hits: list_video_codecs("vulkan") must not return software encoders.
    bool only_vulkan = true;
    for (const CodecInfo& c : all) {
        if (!c.hw || c.hw_device != "vulkan") only_vulkan = false;
    }
    check(only_vulkan, "export_sweep_vulkan: exporter vulkan list returns only vulkan encoders");

    if (g_failures == 0 && all_seen) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}