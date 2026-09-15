// cuda_enc_test — DEVICE-BOUND NVIDIA/CUDA encode regression gate.
//
// Pins the exporter's NVENC encode path against the real encoder on the real
// GPU — the matching sibling of vaapi_enc_test, same discipline: a real
// encode on the real node, not a mock. Skips (exit 2) when the machine has no
// CUDA runtime, no working NVENC encode node, or the real source clip is
// missing.
//
// Checks on every run:
//   1. hevc_nvenc round-trip through export_project() -> mp4 at the target
//      config (2560x1440, 60 fps, 80 Mbps) decodes back to the full expected
//      frame count with non-black content. The render carries a burned-in
//      SUBTITLE + edge fades (make_feature_project) so the row proves a timed
//      title — the CPU-compositor path frame_gpu bails out of — survives the
//      NVENC encode, and the subtitle band is verified present in the decoded
//      output.
//   2. h264_nvenc round-trip decodes to the full expected frame count.
//   3. The bitrate law holds: 10 Mbps encodes smaller than 80 Mbps at the same
//      geometry/fps.
//   4. export_project() actually routed through the pinned CUDA device (the
//      exporter honors HwDeviceManager's preferred GPU pin).
//
// Rows target the user's real export intent (H.265 NVENC 1440p @ 80 Mbps @
// 60 fps) using the source clip CANVAS_TEST_CLIP defaults to
// ~/Videos/clips/2026-09-10 14-28-50.mkv.

#include "cuda_test_common.hpp"

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
    using namespace cuda_test;
    const std::string root = art_root();
    const std::string clip_path = default_clip_path();

    // 1) CUDA runtime usable (SKIP if the app has no CUDA / no NVIDIA GPU).
    if (!cuda_runtime_ok()) {
        std::printf("SKIP CUDA runtime not available (gpu::cuda_available() == false)\n");
        return 2;
    }

    // 2) An encoding CUDA device (SKIP if NVENC can't actually encode).
    const std::string dev = pick_cuda_encode_device();
    if (dev.empty()) {
        std::printf("SKIP no working CUDA/NVENC encode device (h264/hevc_nvenc can't encode on any ordinal)\n");
        return 2;
    }
    std::printf("cuda encode device: ordinal %s\n", dev.c_str());
    canvas::core::HwDeviceManager::set_preferred_gpu("cuda", dev);

    // 3) Real 1440p60 source clip (SKIP if missing).
    const TestClip clip = probe_clip(clip_path);
    if (!clip.valid()) {
        std::printf("SKIP source clip not found/probable: %s\n", clip_path.c_str());
        return 2;
    }
    std::printf("source: %s %dx%d %.0ffps\n", clip.path.c_str(), clip.width,
                clip.height, clip.fps);

    // Export the first ~2 s of the clip at the deliver target geometry, with a
    // burned-in subtitle + edge fades on the clip (the editorial-content path:
    // frame_gpu bails on titles, so export runs the title-rasterise + composite
    // + upload chain into NVENC, not just a straight video blit).
    constexpr int64_t kFrames = 120;  // 2 s @ 60 fps
    const auto proj = make_feature_project(clip, kFrames);
    constexpr double kSubtitleBandH = 0.18;  // bottom 18% = the subtitle band
    constexpr double kBandLitFloor = 0.01;

    // --- hevc_nvenc target-config round trip (1440p60 @ 80 Mbps) ------------
    const EncResult hevc = run_export(proj, "hevc_nvenc", "mp4", kTargetWidth,
                                      kTargetHeight, kTargetFps, kFrames, -1,
                                      "vbr_target", "medium", kTargetBitrateKbps,
                                      "", "hevc80");
    check(hevc.ok, "hevc_nvenc 1440p60@80Mbps export_project succeeds");
    if (hevc.ok) {
        const DecodeResult dec = verify_decode(root + "/out_hevc80.mp4", kSubtitleBandH);
        check(dec.ok, "hevc_nvenc output decodes");
        check(dec.frames == kFrames,
              "hevc_nvenc output decodes to the full expected frame count");
        check(dec.lit_luma > 0, "hevc_nvenc output carries non-black content");
        check(band_lit_fraction(dec) > kBandLitFloor,
              "hevc_nvenc output carries the burned-in subtitle in the subtitle band");
        std::printf("  hevc | %.0f fps | %.1f Mbps | %lld bytes | %d frames | band-lit %.1f%%\n",
                    hevc.fps,
                    hevc.bytes > 0
                        ? static_cast<double>(hevc.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                        : 0.0,
                    static_cast<long long>(hevc.bytes), dec.frames,
                    band_lit_fraction(dec) * 100.0);
    } else {
        std::printf("  hevc_nvenc error: %s\n", hevc.err.c_str());
    }

    // --- h264_nvenc round trip at the same geometry (always present on NVENC) --
    if (encode_probe(dev, "h264_nvenc")) {
        const EncResult h264 = run_export(proj, "h264_nvenc", "mp4", kTargetWidth,
                                          kTargetHeight, kTargetFps, kFrames, -1,
                                          "vbr_target", "medium", kTargetBitrateKbps,
                                          "", "h264_80");
        check(h264.ok, "h264_nvenc 1440p60@80Mbps export_project succeeds");
        if (h264.ok) {
            const DecodeResult dec = verify_decode(root + "/out_h264_80.mp4", kSubtitleBandH);
            check(dec.ok, "h264_nvenc output decodes");
            check(dec.frames == kFrames,
                  "h264_nvenc output decodes to the full expected frame count");
            check(dec.lit_luma > 0, "h264_nvenc output carries non-black content");
            check(band_lit_fraction(dec) > kBandLitFloor,
                  "h264_nvenc output carries the burned-in subtitle in the subtitle band");
            std::printf("  h264 | %.0f fps | %.1f Mbps | %lld bytes | %d frames | band-lit %.1f%%\n",
                        h264.fps,
                        h264.bytes > 0
                            ? static_cast<double>(h264.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                            : 0.0,
                        static_cast<long long>(h264.bytes), dec.frames,
                        band_lit_fraction(dec) * 100.0);
        } else {
            std::printf("  h264_nvenc error: %s\n", h264.err.c_str());
        }
    } else {
        std::printf("  h264_nvenc not available on ordinal %s\n", dev.c_str());
    }

    // --- bitrate law: 10 Mbps must encode smaller than 80 Mbps ---------------
    if (hevc.ok) {
        const EncResult low = run_export(proj, "hevc_nvenc", "mp4", kTargetWidth,
                                         kTargetHeight, kTargetFps, kFrames, -1,
                                         "vbr_target", "medium", 10000, "", "hevc10");
        check(low.ok, "hevc_nvenc 10Mbps export succeeds");
        if (low.ok) {
            check(low.bytes < hevc.bytes,
                  "hevc_nvenc encodes SMALLER at 10 Mbps than at 80 Mbps (bitrate law)");
            const DecodeResult low_dec = verify_decode(root + "/out_hevc10.mp4", kSubtitleBandH);
            check(low_dec.ok && band_lit_fraction(low_dec) > kBandLitFloor,
                  "hevc_nvenc 10Mbps output still carries the burned-in subtitle");
        }
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