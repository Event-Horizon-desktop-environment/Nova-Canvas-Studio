// vaapi_enc_bench — VAAPI encode speed sweep @ the deliver target.
//
// Measures export_project() wall-throughput at the user's real export intent —
// H.265 VAAPI, 2560x1440, 60 fps, 80 Mbps — from the real 1440p AV1 source
// clip, sweeping the knob space that the exporter actually exposes. Prints a
// ranked leaderboard plus bytes/Mbps for each row.
//
// Informational by design: PASS when every measured row produced a decodable,
// non-empty output file — it never asserts a speed (encode throughput is
// machine/driver-dependent). The point is to (re)produce the "fastest safe
// config" evidence (docs/vaapi.md §10) on THIS generation of hardware.
//
// Rows (hevc, the deliver codec):
//   baseline        no extra knobs, rc = vbr_target @ 80 Mbps
//   async_depth     {4, 8, 16, 32, 64} on top of baseline
//   rc mode         CBR vs VBR at 80 Mbps
//   preset names    ultrafast / faster / medium / slow / placebo
//                   (exercises the exporter's vaapi_speed_for wiring)
// h264 reference:   two rows at the same geometry so H.264 vs H.265 speed is
//                   visible on the same run.

#include "vaapi_test_common.hpp"

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
        std::printf("SKIP no working VAAPI encode node\n");
        return 2;
    }
    std::printf("encode node: %s\n", node.c_str());
    canvas::core::HwDeviceManager::set_preferred_gpu("vaapi", device_arg_for(node));

    constexpr int64_t kFrames = 90;  // 1.5 s @ 60 fps per row
    const auto proj = make_single_clip_project(clip, kFrames);

    const auto hevc_row = [&](const std::string& rc_mode, const std::string& preset,
                              const std::string& extra, const std::string& tag,
                              const std::string& label) -> Row {
        Row r{tag, label};
        const EncResult res = run_export(proj, "hevc_vaapi", "mp4", kTargetWidth,
                                         kTargetHeight, kTargetFps, kFrames, -1,
                                         rc_mode, preset, kTargetBitrateKbps, extra, tag);
        r.fps = res.fps;
        r.bytes = res.bytes;
        r.ok = res.ok;
        r.note = res.err;
        r.mbps = r.bytes > 0
                     ? static_cast<double>(r.bytes) * 8.0 / kFrames / kTargetFps / 1000.0
                     : 0.0;
        return r;
    };

    const bool hevc_ok = encode_probe(node, "hevc_vaapi");
    const bool h264_ok = encode_probe(node, "h264_vaapi");
    std::printf("hevc_vaapi encode cap: %s | h264_vaapi: %s\n\n",
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
    for (const char* pres : {"ultrafast", "faster", "slow", "placebo"}) {
        const std::string tag = std::string("hevc_ps_") + pres;
        const std::string label = std::string("hevc VBR@80M preset=") + pres;
        rows.push_back(hevc_row("vbr_target", pres, "", tag, label));
    }

    // --- h264 reference (same geometry/bitrate) ------------------------------
    if (h264_ok) {
        {
            Row r{"h264_base", "h264 VBR@80M baseline"};
            const EncResult res = run_export(proj, "h264_vaapi", "mp4", kTargetWidth,
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
            const EncResult res = run_export(proj, "h264_vaapi", "mp4", kTargetWidth,
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
            const DecodeResult dec = verify_decode(root + "/out_" + row.tag + ".mp4");
            if (!dec.ok || dec.lit_luma == 0) {
                row.ok = false;
                row.note = "output fails decode / black";
            } else {
                row.note += std::string(" (") + std::to_string(dec.frames) + " frames)";
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
        std::printf("recommended ExportSettings: hevc_vaapi 2560x1440 @60fps "
                    "80Mbps vbr_target, preset=medium, extra=\"\"\n");
    }

    if (g_failures == 0) {
        std::printf("\nPASS\n");
        return 0;
    }
    std::printf("\nFAIL (%d)\n", g_failures);
    return 1;
}