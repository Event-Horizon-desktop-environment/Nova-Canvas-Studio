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

}

int main() {
    using namespace vaapi_test;
    const std::string root = art_root();
    const std::string clip_path = default_clip_path();

    const TestClip clip = probe_clip(clip_path);
    if (!clip.valid()) {
        std::printf("SKIP source clip not found/probable: %s\n", clip_path.c_str());
        return 2;
    }
    std::printf("source: %s %dx%d %.0ffps\n", clip.path.c_str(), clip.width,
                clip.height, clip.fps);

    const std::string node = pick_vaapi_encode_node();
    if (node.empty()) {
        std::printf("SKIP no working VAAPI encode node (h264_vaapi can't encode on any /dev/dri/renderD*)\n");
        return 2;
    }
    std::printf("encode node: %s\n", node.c_str());
    canvas::core::HwDeviceManager::set_preferred_gpu("vaapi", device_arg_for(node));

    constexpr int64_t kFrames = 120;
    const auto proj = make_feature_project(clip, kFrames);
    constexpr double kSubtitleBandH = 0.18;
    constexpr double kBandLitFloor = 0.01;

    const EncResult hevc = run_export(proj, "hevc_vaapi", "mp4", kTargetWidth,
                                      kTargetHeight, kTargetFps, kFrames, -1,
                                      "vbr_target", "medium", kTargetBitrateKbps,
                                      "", "hevc80");
    check(hevc.ok, "hevc_vaapi 1440p60@80Mbps export_project succeeds");
    if (hevc.ok) {
        const DecodeResult dec = verify_decode(root + "/out_hevc80.mp4", kSubtitleBandH);
        check(dec.ok, "hevc_vaapi output decodes");
        check(dec.frames == kFrames,
              "hevc_vaapi output decodes to the full expected frame count");
        check(dec.lit_luma > 0, "hevc_vaapi output carries non-black content");
        check(band_lit_fraction(dec) > kBandLitFloor,
              "hevc_vaapi output carries the burned-in subtitle in the subtitle band");
        std::printf("  hevc | %.0f fps | %.1f Mbps | %lld bytes | %d frames | band-lit %.1f%%\n",
                    hevc.fps,
                    hevc.bytes > 0
                        ? static_cast<double>(hevc.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                        : 0.0,
                    static_cast<long long>(hevc.bytes), dec.frames,
                    band_lit_fraction(dec) * 100.0);
    } else {
        std::printf("  hevc_vaapi error: %s\n", hevc.err.c_str());
    }

    if (encode_probe(node, "h264_vaapi")) {
        const EncResult h264 = run_export(proj, "h264_vaapi", "mp4", kTargetWidth,
                                          kTargetHeight, kTargetFps, kFrames, -1,
                                          "vbr_target", "medium", kTargetBitrateKbps,
                                          "", "h264_80");
        check(h264.ok, "h264_vaapi 1440p60@80Mbps export_project succeeds");
        if (h264.ok) {
            const DecodeResult dec = verify_decode(root + "/out_h264_80.mp4", kSubtitleBandH);
            check(dec.ok, "h264_vaapi output decodes");
            check(dec.frames == kFrames,
                  "h264_vaapi output decodes to the full expected frame count");
            check(dec.lit_luma > 0, "h264_vaapi output carries non-black content");
            check(band_lit_fraction(dec) > kBandLitFloor,
                  "h264_vaapi output carries the burned-in subtitle in the subtitle band");
            std::printf("  h264 | %.0f fps | %.1f Mbps | %lld bytes | %d frames | band-lit %.1f%%\n",
                        h264.fps,
                        h264.bytes > 0
                            ? static_cast<double>(h264.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                            : 0.0,
                        static_cast<long long>(h264.bytes), dec.frames,
                        band_lit_fraction(dec) * 100.0);
        } else {
            std::printf("  h264_vaapi error: %s\n", h264.err.c_str());
        }
    }

    if (hevc.ok) {
        const EncResult low = run_export(proj, "hevc_vaapi", "mp4", kTargetWidth,
                                         kTargetHeight, kTargetFps, kFrames, -1,
                                         "vbr_target", "medium", 10000, "", "hevc10");
        check(low.ok, "hevc_vaapi 10Mbps export succeeds");
        if (low.ok) {
            check(low.bytes < hevc.bytes,
                  "hevc_vaapi encodes SMALLER at 10 Mbps than at 80 Mbps (bitrate law)");
            const DecodeResult low_dec = verify_decode(root + "/out_hevc10.mp4", kSubtitleBandH);
            check(low_dec.ok && band_lit_fraction(low_dec) > kBandLitFloor,
                  "hevc_vaapi 10Mbps output still carries the burned-in subtitle");
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
