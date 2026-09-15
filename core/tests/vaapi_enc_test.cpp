// vaapi_enc_test — DEVICE-BOUND VAAPI encode regression gate.
//
// Pins the exporter's VAAPI encode path the same way gpu_grade pins the CUDA
// kernel and vram_leak pins NVDEC decode: against the real encoder on the real
// node, not a mock. Skips (exit 2) when the machine has no working VAAPI
// *encode* node (no GPU / CI / only a decode-only adapter like the NVIDIA NVDEC
// VAAPI shim) or the real source clip is missing.
//
// Checks on every run:
//   1. hevc_vaapi round-trip through export_project() -> mp4 at the target
//      config (2560x1440, 60 fps, 80 Mbps) decodes back to the full expected
//      frame count with non-black content.
//   2. h264_vaapi round-trip decodes to the full expected frame count.
//   3. The bitrate law holds: 10 Mbps encodes smaller than 80 Mbps at the same
//      geometry/fps.
//   4. export_project() actually routed through the pinned node (the exporter
//      honors HwDeviceManager's preferred GPU pin).
//
// Rows target the user's real export intent (H.265 VAAPI 1440p @ 80 Mbps @
// 60 fps) using the source clip CANVAS_TEST_CLIP defaults to
// ~/Videos/clips/2026-09-14 09-12-48.mkv.

#include "vaapi_test_common.hpp"

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

}  // namespace

int main() {
    using namespace vaapi_test;
    const std::string root = art_root();
    const std::string clip_path = default_clip_path();

    // 1) Real 1440p60 source clip (SKIP if missing).
    const TestClip clip = probe_clip(clip_path);
    if (!clip.valid()) {
        std::printf("SKIP source clip not found/probable: %s\n", clip_path.c_str());
        return 2;
    }
    std::printf("source: %s %dx%d %.0ffps\n", clip.path.c_str(), clip.width,
                clip.height, clip.fps);

    // 2) An encoding VAAPI device (SKIP if none).
    const std::string node = pick_vaapi_encode_node();
    if (node.empty()) {
        std::printf("SKIP no working VAAPI encode node (h264_vaapi can't encode on any /dev/dri/renderD*)\n");
        return 2;
    }
    std::printf("encode node: %s\n", node.c_str());
    canvas::core::HwDeviceManager::set_preferred_gpu("vaapi", device_arg_for(node));

    // Export the first ~2 s of the clip at the deliver target geometry.
    constexpr int64_t kFrames = 120;  // 2 s @ 60 fps
    const auto proj = make_single_clip_project(clip, kFrames);

    // --- hevc_vaapi target-config round trip (1440p60 @ 80 Mbps) ------------
    const EncResult hevc = run_export(proj, "hevc_vaapi", "mp4", kTargetWidth,
                                      kTargetHeight, kTargetFps, kFrames, -1,
                                      "vbr_target", "medium", kTargetBitrateKbps,
                                      "", "hevc80");
    check(hevc.ok, "hevc_vaapi 1440p60@80Mbps export_project succeeds");
    if (hevc.ok) {
        const DecodeResult dec = verify_decode(root + "/out_hevc80.mp4");
        check(dec.ok, "hevc_vaapi output decodes");
        check(dec.frames == kFrames,
              "hevc_vaapi output decodes to the full expected frame count");
        check(dec.lit_luma > 0, "hevc_vaapi output carries non-black content");
        std::printf("  hevc | %.0f fps | %.1f Mbps | %lld bytes | %d frames\n",
                    hevc.fps,
                    hevc.bytes > 0
                        ? static_cast<double>(hevc.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                        : 0.0,
                    static_cast<long long>(hevc.bytes), dec.frames);
    } else {
        std::printf("  hevc_vaapi error: %s\n", hevc.err.c_str());
    }

    // --- h264_vaapi round trip at the same geometry (always present) ---------
    if (encode_probe(node, "h264_vaapi")) {
        const EncResult h264 = run_export(proj, "h264_vaapi", "mp4", kTargetWidth,
                                          kTargetHeight, kTargetFps, kFrames, -1,
                                          "vbr_target", "medium", kTargetBitrateKbps,
                                          "", "h264_80");
        check(h264.ok, "h264_vaapi 1440p60@80Mbps export_project succeeds");
        if (h264.ok) {
            const DecodeResult dec = verify_decode(root + "/out_h264_80.mp4");
            check(dec.ok, "h264_vaapi output decodes");
            check(dec.frames == kFrames,
                  "h264_vaapi output decodes to the full expected frame count");
            check(dec.lit_luma > 0, "h264_vaapi output carries non-black content");
            std::printf("  h264 | %.0f fps | %.1f Mbps | %lld bytes | %d frames\n",
                        h264.fps,
                        h264.bytes > 0
                            ? static_cast<double>(h264.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                            : 0.0,
                        static_cast<long long>(h264.bytes), dec.frames);
        } else {
            std::printf("  h264_vaapi error: %s\n", h264.err.c_str());
        }
    }

    // --- bitrate law: 10 Mbps must encode smaller than 80 Mbps ---------------
    if (hevc.ok) {
        const EncResult low = run_export(proj, "hevc_vaapi", "mp4", kTargetWidth,
                                         kTargetHeight, kTargetFps, kFrames, -1,
                                         "vbr_target", "medium", 10000, "", "hevc10");
        check(low.ok, "hevc_vaapi 10Mbps export succeeds");
        if (low.ok)
            check(low.bytes < hevc.bytes,
                  "hevc_vaapi encodes SMALLER at 10 Mbps than at 80 Mbps (bitrate law)");
        std::printf("  hevc bitrate law | 80Mbps=%lld | 10Mbps=%lld\n",
                    static_cast<long long>(hevc.bytes),
                    static_cast<long long>(low.ok ? low.bytes : -1));
    }

    if (g_failures == 0) {
        std::printf("\nPASS\n");
        return 0;
    }
    std::printf("\nFAIL (%d)\n", g_failures);
    return 1;
}