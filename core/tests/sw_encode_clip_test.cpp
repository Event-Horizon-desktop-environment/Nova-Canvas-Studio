#include "vaapi_test_common.hpp"

#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

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

}

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

    {
        check(clip.width == 2560, "encode target width == 2560");
        check(clip.height == 1440, "encode target height == 1440");
        check(std::fabs(clip.fps - 60.0) < 0.01, "encode target fps ~= 60");
        check(clip.total_frames > 0, "encode target total_frames > 0");
        std::printf("       pure target: %dx%d @ %.2f fps, ~%" PRId64 " frames\n",
                    clip.width, clip.height, clip.fps, clip.total_frames);
    }

    constexpr double kFps = 60.0;
    constexpr int64_t kFrames = 180;
    const Project proj = vaapi_test::make_single_clip_project(clip, kFrames);

    const vaapi_test::EncResult r = vaapi_test::run_export(
        proj, "libx265", "mp4", 2560, 1440, kFps, kFrames, -1, "cbr", "veryfast",
        80000, "x265-params=pools=+,-,-,-:frame-threads=0:lookahead-slices=8",
        "sw_en_1440p60_80m");
    check(r.ok, "libx265 1440p60@80 Mbps cpu export succeeds");
    if (r.ok) {
        std::printf("       libx265 encode: %.1f fps, %.1f ms, %lld bytes\n",
                    r.fps, r.ms, static_cast<long long>(r.bytes));

        const vaapi_test::DecodeResult dec =
            vaapi_test::verify_decode(vaapi_test::art_root() + "/out_sw_en_1440p60_80m.mp4");
        check(dec.ok, "libx265 cpu output decodes back");
        check(dec.frames == kFrames, "libx265 cpu output carries full frame count");
        check(dec.lit_luma > 0, "libx265 cpu output carries non-black content");
        std::printf("       decode-back: %d frames, %lld lit luma samples\n",
                    dec.frames, static_cast<long long>(dec.lit_luma));
    } else {
        std::printf("       libx265 cpu error: %s\n", r.err.c_str());
    }

    if (failures != 0) {
        std::fprintf(stderr, "sw_encode_clip_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("sw_encode_clip_test: all checks passed\n");
    return 0;
}
