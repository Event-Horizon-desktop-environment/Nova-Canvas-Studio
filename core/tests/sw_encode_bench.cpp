// Software (CPU-only) libx265 preset sweep at the deliver law: 2560x1440,
// 60 fps, 80,000 kbps, threads = 0 (FFmpeg auto = all cores), bounded 180-f
// head window of the real 1440p60 reel.
//
// INFORMATIONAL ONLY (mirrors the vaapi_enc_bench convention): validates every
// row's output decodes back with full frame count + content, but never asserts
// speed — the numbers pick the fastest preset the gate test pins. At a fixed
// 80 Mbps target the bitrate (not the preset) sets delivered quality, so rows
// are comparable by fps as long as bytes stay near target and decode-back is
// clean. SKIPs (exit 2) when the clip is absent or libx265 is unavailable.

#include "vaapi_test_common.hpp"

#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace canvas::core;

namespace {

std::string default_clip_path() {
    if (const char* p = std::getenv("CANVAS_TEST_CLIP")) return p;
    return "/home/matt/Videos/clips/2026-09-14 09-12-48.mkv";
}

}  // namespace

int main() {
    const std::string path = default_clip_path();
    const vaapi_test::TestClip clip = vaapi_test::probe_clip(path);
    if (!clip.valid()) {
        std::printf("skipping: clip not present (%s)\n", path.c_str());
        return 2;
    }
    if (!avcodec_find_encoder_by_name("libx265")) {
        std::printf("skipping: libx265 encoder unavailable\n");
        return 2;
    }

    constexpr double kFps = 60.0;
    constexpr int64_t kFrames = 180;  // 3 s @ 60 fps
    const Project proj = vaapi_test::make_single_clip_project(clip, kFrames);

    struct Row {
        const char* preset;
        const char* extra;
        const char* tag;
    };
    const Row rows[] = {
        {"medium", "", "sw_bench_medium"},
        {"fast", "", "sw_bench_fast"},
        {"faster", "", "sw_bench_faster"},
        {"veryfast", "", "sw_bench_veryfast"},
        // veryfast + explicit thread pools: frame-level + WPP parallelism maxed,
        // lookahead kept so rate control (and hence quality at 80 Mbps) holds.
        {"veryfast", "x265-params=pools=+,-,-,-:frame-threads=0:lookahead-slices=8",
         "sw_bench_veryfast_pools"},
        // ... + pmode/pme: distributed mode-decision and motion estimation.
        // MEASURED SLOWER (36.2 vs 44.4 fps): sync overhead exceeds gains at
        // this frame count / core count. Kept as a documented dead end.
        {"veryfast",
         "x265-params=pools=+,-,-,-:frame-threads=0:lookahead-slices=8:pmode=1:pme=1",
         "sw_bench_veryfast_pmode"},
        // ... + HandBrake-style long GOP (keyint=600 = 10 s @ 60 fps,
        // min-keyint=1 s): fewer IDR anchors per window -> less forced
        // intra work and better compression efficiency. MEASURED NO WIN
        // (42.6 vs 42.5 fps = noise, and +7% bytes on the 3 s window from
        // RC-startup transients). Kept as a documented dead end.
        {"veryfast",
         "x265-params=pools=+,-,-,-:frame-threads=0:lookahead-slices=8:keyint=600:min-keyint=60",
         "sw_bench_veryfast_keyint"},
        // ... + near-free mode-decision prunes: skip improbable intra/merge
        // candidates early (limit-modes/refs/tu) and drop rectangular + asymmetric
        // partitions. MEASURED NO WIN (43.1 vs 42.7 fps = run noise, and it
        // spends a little efficiency for nothing). Kept as a documented dead end.
        {"veryfast",
         "x265-params=pools=+,-,-,-:frame-threads=0:lookahead-slices=8:limit-modes=1:limit-refs=3:limit-tu=4:rect=0:amp=0",
         "sw_bench_veryfast_prune"},
        // ... + explicit frame-thread count on top of the prune set. MEASURED
        // NO WIN (43.0 fps = same as auto): x265's auto pick already saturates
        // 24 cores. Kept as a documented dead end.
        {"veryfast",
         "x265-params=pools=+,-,-,-:frame-threads=4:lookahead-slices=8:limit-modes=1:limit-refs=3:limit-tu=4:rect=0:amp=0",
         "sw_bench_veryfast_ft4"},
    };

    std::printf("%-16s %8s %10s %12s %10s %7s %7s\n", "preset", "fps", "ms",
                "bytes", "Mbps", "frames", "lit");
    double best_fps = 0.0;
    const char* best_preset = "";
    for (const Row& row : rows) {
        const vaapi_test::EncResult r = vaapi_test::run_export(
            proj, "libx265", "mp4", 2560, 1440, kFps, kFrames, -1, "cbr",
            row.preset, 80000, row.extra, row.tag);
        if (!r.ok) {
            std::printf("%-16s FAILED: %s\n", row.preset, r.err.c_str());
            continue;
        }
        const vaapi_test::DecodeResult dec = vaapi_test::verify_decode(
            vaapi_test::art_root() + "/out_" + row.tag + ".mp4");
        const double mbps = r.bytes > 0
                                ? static_cast<double>(r.bytes) * 8.0 /
                                      (static_cast<double>(kFrames) / kFps) / 1e6
                                : 0.0;
        std::printf("%-16s %8.1f %10.1f %12lld %10.1f %7d %7lld%s\n", row.preset,
                    r.fps, r.ms, static_cast<long long>(r.bytes), mbps,
                    dec.frames, static_cast<long long>(dec.lit_luma),
                    (dec.ok && dec.frames == kFrames && dec.lit_luma > 0)
                        ? ""
                        : "   <-- DECODE-BACK BAD");
        if (dec.ok && dec.frames == kFrames && dec.lit_luma > 0 &&
            r.fps > best_fps) {
            best_fps = r.fps;
            best_preset = row.preset;
        }
    }
    std::printf("sw_encode_bench: fastest clean row: %s @ %.1f fps\n", best_preset,
                best_fps);
    return 0;
}
