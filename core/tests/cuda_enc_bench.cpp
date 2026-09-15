// cuda_enc_bench — NVIDIA/CUDA (NVENC) encode speed sweep @ the deliver target.
//
// Measures export_project() wall-throughput at the user's real export intent —
// H.265 NVENC, 2560x1440, 60 fps, 80 Mbps — from the real 1440p AV1 source,
// sweeping the knob space that the exporter actually exposes. Prints a ranked
// leaderboard plus bytes/Mbps for each row. The sibling of vaapi_enc_bench for
// the CUDA backend.
//
// Informational by design: PASS when every measured row produced a decodable,
// non-empty output file — it never asserts a speed. Rows:
//   hevc async_depth    {4, 8, 16, 32, 64} on top of baseline (VBR@80 Mbps)
//   rc mode             CBR vs VBR at 80 Mbps
//   preset names        ultrafast / faster / medium / slow / placebo
//                       (exercises the exporter's nv_preset_for p1..p7 wiring)
//   feature rows        baseline + burned-in subtitle/fades. For NVENC the title
//                       stays on the GPU and is fused by the nv12TitleBlend
//                       kernel launch that already does resize+grade, so these
//                       rows are the deliver numbers a subtitle-bearing timeline
//                       gets on the fast path, not a CPU-compositor baseline.
//   h264 reference      same geometry so H.264 vs H.265 speed is visible.

#include "cuda_test_common.hpp"

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

struct Row {
    std::string tag;    // filename tag (out_<tag>.mp4)
    std::string label;  // human-readable row label
    double fps = 0.0;
    double mbps = 0.0;
    int64_t bytes = 0;
    bool ok = false;
    std::string note;
};

}  // namespace

int main() {
    using namespace cuda_test;
    const std::string root = art_root();
    const std::string clip_path = default_clip_path();

    if (!cuda_runtime_ok()) {
        std::printf("SKIP CUDA runtime not available\n");
        return 2;
    }
    const std::string dev = pick_cuda_encode_device();
    if (dev.empty()) {
        std::printf("SKIP no working CUDA/NVENC encode device\n");
        return 2;
    }
    std::printf("cuda encode device: ordinal %s\n", dev.c_str());
    canvas::core::HwDeviceManager::set_preferred_gpu("cuda", dev);

    const TestClip clip = probe_clip(clip_path);
    if (!clip.valid()) {
        std::printf("SKIP source clip not found/probable: %s\n", clip_path.c_str());
        return 2;
    }
    std::printf("source: %s %dx%d %.0ffps\n", clip.path.c_str(), clip.width,
                clip.height, clip.fps);

    constexpr int64_t kFrames = 90;  // 1.5 s @ 60 fps per row
    const auto proj = make_single_clip_project(clip, kFrames);
    // Editorial-content variant: burn-in subtitle + edge fades. On the CUDA path
    // the per-frame title sprite is uploaded once and woven into the fused
    // resize+grade kernel launch (nv12TitleBlend), so these rows measure the full
    // title raster->blend->encode deliver path at GPU cost, not a bare blit.
    const auto feat_proj = make_feature_project(clip, kFrames);
    constexpr double kSubtitleBandH = 0.18;  // bottom 18% = the subtitle band

    const auto hevc_row = [&](const std::string& rc_mode, const std::string& preset,
                              const std::string& extra, const std::string& tag,
                              const std::string& label, bool feature = false) -> Row {
        Row r{tag, label};
        const EncResult res = run_export(feature ? feat_proj : proj, "hevc_nvenc", "mp4",
                                         kTargetWidth, kTargetHeight, kTargetFps,
                                         kFrames, -1, rc_mode, preset,
                                         kTargetBitrateKbps, extra, tag);
        r.fps = res.fps;
        r.bytes = res.bytes;
        r.ok = res.ok;
        r.note = res.err + (feature ? " [content: title+fades]" : "");
        r.mbps = r.bytes > 0
                     ? static_cast<double>(r.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                     : 0.0;
        return r;
    };

    const bool hevc_ok = encode_probe(dev, "hevc_nvenc");
    const bool h264_ok = encode_probe(dev, "h264_nvenc");
    std::printf("hevc_nvenc encode cap: %s | h264_nvenc: %s\n\n",
                hevc_ok ? "yes" : "no", h264_ok ? "yes" : "no");

    std::vector<Row> rows;

    // --- hevc: baseline + knob sweep -----------------------------------------
    rows.push_back(hevc_row("vbr_target", "medium", "", "hevc_base", "hevc VBR@80M baseline"));
    for (const char* depth : {"4", "8", "16", "32", "64"}) {
        const std::string tag = std::string("hevc_ad") + depth;
        const std::string label = std::string("hevc VBR@80M async_depth=") + depth;
        rows.push_back(hevc_row("vbr_target", "medium",
                                std::string("async_depth=") + depth + "\n", tag, label));
    }
    rows.push_back(hevc_row("cbr", "medium", "", "hevc_cbr", "hevc CBR@80M (tight VBV)"));
    for (const char* pres : {"ultrafast", "faster", "medium", "slow", "placebo"}) {
        const std::string tag = std::string("hevc_ps_") + pres;
        const std::string label = std::string("hevc VBR@80M preset=") + pres;
        rows.push_back(hevc_row("vbr_target", pres, "", tag, label));
    }

    // --- feature-content rows: subtitle + fades through the deliver path -----
    // Exercising the same fused GPU title->encode chain the app runs (the
    // sprite upload + nv12TitleBlend fusion happen inside the export session).
    rows.push_back(hevc_row("vbr_target", "medium", "", "hevc_feat",
                            "hevc VBR@80M baseline + subtitle/fades", true));
    rows.push_back(hevc_row("vbr_target", "ultrafast", "", "hevc_feat_fast",
                            "hevc VBR@80M ultrafast + subtitle/fades", true));

    // --- h264 reference (same geometry/bitrate) ------------------------------
    if (h264_ok) {
        {
            Row r{"h264_base", "h264 VBR@80M baseline"};
            const EncResult res = run_export(proj, "h264_nvenc", "mp4", kTargetWidth,
                                             kTargetHeight, kTargetFps, kFrames, -1,
                                             "vbr_target", "medium", kTargetBitrateKbps,
                                             "", "h264_base");
            r.fps = res.fps; r.bytes = res.bytes; r.ok = res.ok; r.note = res.err;
            r.mbps = r.bytes > 0
                         ? static_cast<double>(r.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                         : 0.0;
            rows.push_back(std::move(r));
        }
        {
            Row r{"h264_ad16", "h264 VBR@80M async_depth=16"};
            const EncResult res = run_export(proj, "h264_nvenc", "mp4", kTargetWidth,
                                             kTargetHeight, kTargetFps, kFrames, -1,
                                             "vbr_target", "medium", kTargetBitrateKbps,
                                             "async_depth=16\n", "h264_ad16");
            r.fps = res.fps; r.bytes = res.bytes; r.ok = res.ok; r.note = res.err;
            r.mbps = r.bytes > 0
                         ? static_cast<double>(r.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                         : 0.0;
            rows.push_back(std::move(r));
        }
    }

    // --- validate + rank -----------------------------------------------------
    std::printf("%-42s | %6s | %6s | %10s | %s\n", "row", "fps", "Mbps", "bytes", "status");
    for (auto& row : rows) {
        if (row.ok) {
            const bool feature = row.note.find("[content: title+fades]") != std::string::npos;
            const DecodeResult dec =
                verify_decode(root + "/out_" + row.tag + ".mp4",
                              feature ? kSubtitleBandH : 0.0);
            if (!dec.ok || dec.lit_luma == 0) {
                row.ok = false;
                row.note = "output fails decode / black";
            } else {
                row.note += std::string(" (") + std::to_string(dec.frames) + " frames";
                if (feature) {
                    const double frac = band_lit_fraction(dec);
                    row.note += ", band-lit " +
                                std::string(frac > 0.01 ? "yes" : "MISSING") +
                                " (" + std::to_string(static_cast<int>(frac * 100.0)) +
                                "%)";
                    if (frac <= 0.01) {
                        row.ok = false;
                        row.note += " :: burned-in subtitle dropped";
                    }
                }
                row.note += ")";
            }
        }
        std::printf("%-42s | %5.0f | %5.2f | %10lld | %s\n", row.label.c_str(), row.fps,
                    row.mbps, static_cast<long long>(row.bytes),
                    row.ok ? "ok" : "FAIL");
    }

    for (const auto& row : rows) {
        if (row.ok)
            check(true, row.tag.c_str());
        else
            check(false, (row.tag + " :: " + row.note).c_str());
    }

    const auto best = std::max_element(rows.begin(), rows.end(),
                                       [](const Row& a, const Row& b) { return a.fps < b.fps; });
    if (best != rows.end()) {
        std::printf("\nfastest: %s at %.0f fps (%.1f Mbps, %lld bytes)\n",
                    best->tag.c_str(), best->fps, best->mbps,
                    static_cast<long long>(best->bytes));
        std::printf("recommended ExportSettings: hevc_nvenc 2560x1440 @60fps "
                    "80Mbps vbr_target, preset=medium, extra=\"\"\n");
    }

    if (g_failures == 0) {
        std::printf("\nPASS\n");
        return 0;
    }
    std::printf("\nFAIL (%d)\n", g_failures);
    return 1;
}