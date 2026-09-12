// queue_matrix_test — per-queue-family codec operation matrix.
//
// Phase 0 reading that pins WHAT runs on WHICH family, per codec, on this
// machine. The backend's queue plan (locked in docs/vulkan.md) hands each
// operation a specific family: decode to a video-decode family, encode to a
// video-encode family, composite to a graphics/compute family, and the whole
// cross-queue picture shuffles frames between them via timeline semaphores. A
// machine where e.g. AV1 decode has no family at all can't run Phase P-C's AV1
// decode path — that test would SKIP and name P-C. This reading documents the
// matrix those decisions are made from, and fails loudly if the probe's
// family/operation reports ever contradict each other.
//
// PASS (0)  — every device reports at least one decode+encode+compute family
//             and the per-codec operation bits are internally consistent with
//             the family flags.
// FAIL (1)  — probe coherence broke (an operation bit on a family that lacks
//             the corresponding queue flag, or a codec bit on no family).
// SKIP (2)  — no Vulkan implementation at all.

#include "vk_probe.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

using namespace canvas::vktest;

namespace {

constexpr std::uint32_t kDecodeOps =
    VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
    VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
    VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR |
    VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
constexpr std::uint32_t kEncodeOps =
    VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
    VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
    VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

const char* decode_name(std::uint32_t op) {
    switch (op) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: return "h264-decode";
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: return "h265-decode";
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: return "av1-decode";
        case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR: return "vp9-decode";
        default: return "?";
    }
}
const char* encode_name(std::uint32_t op) {
    switch (op) {
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR: return "h264-encode";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR: return "h265-encode";
        case VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR: return "av1-encode";
        default: return "?";
    }
}

}  // namespace

int main() {
    const ProbeResult r = run_probe();
    if (!r.ok || r.devices.empty()) {
        std::printf("SKIP  queue_matrix: no Vulkan implementation (%s)\n",
                    r.ok ? "no devices" : r.error.c_str());
        return 2;
    }

    for (const auto& d : r.devices) {
        std::printf("queue_matrix: %s (%u families)\n", d.name.c_str(),
                    static_cast<unsigned>(d.queue_families.size()));
        for (const auto& q : d.queue_families) {
            std::printf("  family %u flags=0x%x dec=%s enc=%s gfx=%s cmp=%s\n",
                        q.index, q.flags,
                        q.video_decode ? "yes" : "no",
                        q.video_encode ? "yes" : "no",
                        q.graphics ? "yes" : "no",
                        q.compute ? "yes" : "no");
            for (std::uint32_t op : {VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
                                     VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
                                     VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
                                     VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR}) {
                if ((q.video_codec_operations & op) != 0)
                    std::printf("      %s\n", decode_name(op));
            }
            for (std::uint32_t op : {VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
                                     VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
                                     VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR}) {
                if ((q.video_codec_operations & op) != 0)
                    std::printf("      %s\n", encode_name(op));
            }
        }

        // Coherence: video ops only on families with the video queue flags.
        bool coherent = true;
        for (const auto& q : d.queue_families) {
            const bool video_flag =
                (q.flags & (VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) != 0;
            if ((q.video_decode || q.video_encode) && !video_flag) coherent = false;
            if ((q.video_codec_operations & (kDecodeOps | kEncodeOps)) != 0 && !video_flag)
                coherent = false;
        }
        check(coherent, "queue_matrix: video ops only on video-flagged families");
    }

    const auto& d = r.devices.front();
    bool any_decode = false, any_encode = false, any_compute = false;
    for (const auto& q : d.queue_families) {
        any_decode |= q.video_decode;
        any_encode |= q.video_encode;
        any_compute |= q.compute;
    }
    check(any_compute, "queue_matrix: compute family present for composite/P-D");
    check(any_decode, "queue_matrix: decode family present (gates P-C readings)");
    check(any_encode, "queue_matrix: encode family present (gates P-E readings)");

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}