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

}

int main() {
    const std::string path = default_clip_path();

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

        int64_t frames = 0;
        int64_t lit_luma = 0;
        int nonblack = 0;
        for (int64_t i = 0; i < 180; ++i) {
            VideoFramePtr fr = dec.decode_next();
            if (!fr) break;
            ++frames;
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

    {
        VideoDecoder dec;
        std::string err;
        if (!dec.open(path, &err, nullptr)) return 2;

        const double total = static_cast<double>(pr.total_frames);
        const int64_t taps[3] = {
            300,
            static_cast<int64_t>(total * 0.45),
            static_cast<int64_t>(total * 0.85),
        };
        dec.build_iframe_index();
        for (const int64_t tap : taps) {
            VideoFramePtr fr = dec.seek_to_frame_indexed(tap);
            check(fr != nullptr, "indexed seek to tap lands a frame");
            if (fr) {
                const int64_t landed = fr->frame_number;
                check(landed >= 0 && landed <= tap + 240,
                      "indexed seek lands at-or-before tap (+1 GOP tolerance)");
                check(sample_lit_luma(*fr, 16, 32) > 0,
                      "indexed seek frame carries content");
            }
        }
        dec.close();
    }

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
