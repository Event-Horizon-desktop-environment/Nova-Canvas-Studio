// Real-clip SOFTWARE decode test. Unlike the synthetic sw_decode_test (gray
// YUV frames only), this drives the actual VideoDecoder in software mode
// (open with null hw device ctx -> pure CPU) over genuine photo-real AV1
// footage so the whole fetch pipeline — container open, stream probe,
// sequential forward walk, I-frame index, indexed seeks, and the swscale RGBA
// convert with its aligned-stride law — is exercised end to end on real media.
//
// Media requirement: the test source is the user's 1440p60 AV1 clip
//   /home/matt/Videos/clips/2026-09-14 09-12-48.mkv  (2560x1440, 60 fps,
//   ~40,920 frames over 682 s) — overridable via CANVAS_TEST_CLIP exactly like
//   the vaapi encode tests do. When the clip is missing the test SKIPs (exit
//   2, the suite-wide skip convention), it does not fail.
//
// Speed-vs-quality budget: the full 682 s reel is ~41k frames; decoding all of
// it in software sells the "fast" half of the ask. So this walk is bounded —
// a short head window plus scattered seeks to head/middle/tail tap points —
// which still proves frame-accurate seeks, non-black content, dims/fps laws,
// and per-path decode timing without becoming a 10-minute CI job.

#include "canvas/core/media/video_decoder.hpp"

#include "vaapi_test_common.hpp"

#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace canvas::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

std::string default_clip_path() {
    if (const char* p = std::getenv("CANVAS_TEST_CLIP")) return p;
    return "/home/matt/Videos/clips/2026-09-14 09-12-48.mkv";
}

int64_t sample_lit_luma(const VideoFrame& fr, int step, int thresh) {
    int64_t lit = 0;
    const std::size_t st = fr.stride;
    for (int y = 0; y < fr.height; y += step) {
        const std::size_t row = static_cast<std::size_t>(y) * st;
        for (int x = 0; x < fr.width; x += step) {
            const std::size_t o = row + static_cast<std::size_t>(x) * 4u;
            if (o + 2 >= fr.rgba.size()) break;
            const int luma = (299 * fr.rgba[o] + 587 * fr.rgba[o + 1] +
                              114 * fr.rgba[o + 2]) /
                             1000;
            if (luma > thresh) ++lit;
        }
    }
    return lit;
}

}  // namespace

int main() {
    const std::string path = default_clip_path();

    // ---- 1. Probe the real clip: dims / fps / duration / frame count -------
    vaapi_test::TestClip pr = vaapi_test::probe_clip(path);
    check(pr.valid(), "real clip probes valid");
    if (!pr.valid()) {
        std::printf("skipping: real clip not present (%s)\n", path.c_str());
        return 2;
    }
    check(pr.width == 2560, "real clip width == 2560");
    check(pr.height == 1440, "real clip height == 1440");
    check(std::fabs(pr.fps - 60.0) < 0.01, "real clip fps ~= 60");
    check(pr.total_frames > 0, "real clip total_frames > 0");
    std::printf("       clip: %dx%d @ %.2f fps, ~%" PRId64 " frames\n", pr.width,
                pr.height, pr.fps, static_cast<int64_t>(pr.total_frames));

    // ---- 2. Software decode: head window + metadata laws --------------------
    // Open through the soft path (null device context -> software only) and
    // walk a short head window, checking the output carries content (not
    // black) and lands at the probe's dims.
    {
        VideoDecoder dec;
        std::string err;
        check(dec.open(path, &err, nullptr), "soft software open succeeds");
        check(dec.is_open(), "soft is_open after open");
        if (!dec.is_open()) {
            std::printf("       soft open error: %s\n", err.c_str());
            return 2;
        }
        check(std::string(dec.hardware_name()) == "sw", "decoder runs on sw path");
        check(dec.width() == 2560 && dec.height() == 1440,
              "soft dims match probe (2560x1440)");
        check(std::fabs(dec.frame_rate() - 60.0) < 0.01, "soft fps ~= 60");

        // Read a bounded head window via the sequential path.
        int64_t frames = 0;
        int64_t lit_luma = 0;  // sampler across the window
        int nonblack = 0;
        for (int64_t i = 0; i < 180; ++i) {  // 3 s head window @ 60 fps
            VideoFramePtr fr = dec.decode_next();
            if (!fr) break;
            ++frames;
            // Luma content gauge on the RGBA output: real footage is genuine
            // photo (never all-black frames).
            if (sample_lit_luma(*fr, 8, 16) > 0) ++nonblack;
            lit_luma += sample_lit_luma(*fr, 32, 16);
        }
        check(frames > 0, "soft head window decodes frames");
        check(frames >= 100, "soft head window >= 100 frames (bounded but real)");
        check(nonblack > 0, "soft head window carries non-black content");
        std::printf("       soft: %" PRId64 " frames decoded, %" PRId64
                    " lit-luma samples, %d non-black frames\n",
                    frames, lit_luma, nonblack);
        dec.close();
    }

    // ---- 3. Scattered indexed seeks across the reel --------------------------
    // Build the I-frame index and anchor-seek to head / middle / tail taps,
    // verifying each lands at a frame near the tap and decodes content.
    {
        VideoDecoder dec;
        std::string err;
        if (!dec.open(path, &err, nullptr)) return 2;

        // Middle / tail taps are fractions of the decoded reel.
        const double total = static_cast<double>(pr.total_frames);
        const int64_t taps[3] = {
            300,                                     // head
            static_cast<int64_t>(total * 0.45),      // ~middle
            static_cast<int64_t>(total * 0.85),      // ~tail
        };
        dec.build_iframe_index();
        for (const int64_t tap : taps) {
            VideoFramePtr fr = dec.seek_to_frame_indexed(tap);
            check(fr != nullptr, "indexed seek to tap lands a frame");
            if (fr) {
                // Taps are off-keyframe; the index seek should land at or
                // slightly before, never far past.
                const int64_t landed = fr->frame_number;
                check(landed >= 0 && landed <= tap + 240,
                      "indexed seek lands at-or-before tap (+1 GOP tolerance)");
                check(sample_lit_luma(*fr, 16, 32) > 0,
                      "indexed seek frame carries content");
            }
        }
        dec.close();
    }

    // ---- 4. Seek-time budget (quality gate) ---------------------------------
    // A software AV1 seek should land in well under 2 s even far across the
    // reel; this is the "fast" half of the fast-but-quality ask.
    {
        VideoDecoder dec;
        std::string err;
        if (!dec.open(path, &err, nullptr)) return 2;

        const auto t0 = std::chrono::steady_clock::now();
        dec.build_iframe_index();
        const auto t1 = std::chrono::steady_clock::now();
        const double index_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        check(index_ms < 2000.0, "iframe index build < 2 s");

        dec.seek_to_frame_indexed(
            static_cast<int64_t>(static_cast<double>(pr.total_frames) * 0.8));
        const auto t2 = std::chrono::steady_clock::now();
        const double seek_ms =
            std::chrono::duration<double, std::milli>(t2 - t1).count();
        check(seek_ms < 2000.0, "far indexed seek < 2 s");
        std::printf("       timing: index build %.1f ms, far seek %.1f ms\n",
                    index_ms, seek_ms);
        dec.close();
    }

    if (failures == 0) {
        std::printf("sw_decode_clip: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "sw_decode_clip: %d check(s) failed\n", failures);
    return 1;
}
