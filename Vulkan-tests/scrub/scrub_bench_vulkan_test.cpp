// scrub_bench_vulkan_test — VCU scrub-preview latency budget gate (Phase P-C).
//
// Phase 0 placeholder for the reading core's scrub_bench pins on the CPU path:
// p95 scrub-preview latency must stay under budget. The Vulkan backend's scrub
// preview (Phase P-C backend decode) must clear the SAME budget — but there is
// no Vulkan decode yet, so this reading can't run. It exists so the "reading
// never absent" gate holds: it emits the owning phase and SKIPs (exit 2) when
// the linked FFmpeg has no Vulkan decoder registered.
//
// When Phase P-C lands a Vulkan decoder, this test is renamed-first to the real
// measurement (decode N scrub frames, measure p95 against the scrub_bench law)
// — the SKIP path below simply stops matching then.
//
// PASS (0)  — never reached until P-C (would run the real budget gate).
// FAIL (1)  — never reached until P-C.
// SKIP (2)  — linked FFmpeg has no Vulkan decoder; owning phase P-C.

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdio>
#include <cstdlib>

int main() {
    const AVCodec* dec = avcodec_find_decoder_by_name("h264_vulkan");
    if (dec == nullptr) {
        std::printf("SKIP  scrub_bench_vulkan: linked FFmpeg has no h264_vulkan decoder "
                    "(owning phase P-C installs the self-built FFmpeg + decode path)\n");
        return 2;
    }
    // Reached only when the P-C decode path exists; the real p95 budget gate
    // lands with that phase. Refuse silently rather than pretend to measure.
    std::printf("note: scrub_bench_vulkan found h264_vulkan decoder; P-C gate not "
                "implemented yet\n");
    return 2;
}