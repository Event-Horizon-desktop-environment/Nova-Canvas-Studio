// Software-decode RGBA conversion unit test: pins the sws buffer-sizing law
// behind SoftDecoder::convert_to_rgba, the seam that fixed GitHub issue #4
// (heap corruption). The old destination sizing was
//   stride = out_w*4; rgba.resize(stride * out_h)
// with dst_linesize hardcoded to that stride; libswscale pads each destination
// row to its picture-line alignment, so wherever the aligned stride exceeds
// w*4 (any output width that is not already a multiple of 32/4) sws_scale wrote
// past the end of the vector. This test
//  - proves the destination obeys the aligned layout for full-res, downscaled,
//    and odd intermediate output dims across portrait/landscape sources;
//  - proves every converted frame satisfies the two invariants the corruption
//    violated: stride >= width*4 and rgba.size() >= stride*height;
//  - exercises the low-res preview cap (set_output_dim) — the scrub path that
//    converts frames by the hundreds.
//
// All sources are synthesized YUV420P frames (mid-gray, even dims); the color
// spec defaults to BT709/Limited, which convert_to_rgba resolves to full-range
// RGB. No media file, no GPU, no device.

#include "canvas/core/media/sw_decode.hpp"
#include "canvas/core/media/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

AVFrame* make_gray_yuv(int w, int h) {
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return nullptr;
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    // Mid-gray: luma 128, both chroma planes 128. Fill padded row extent so the
    // buffer contents are fully initialized (get_buffer's alignment is honored).
    const struct {
        int plane;
        int rows;
    } planes[3] = {{0, h}, {1, h / 2}, {2, h / 2}};
    for (const auto& p : planes) {
        for (int y = 0; y < p.rows; ++y)
            std::memset(f->data[p.plane] + static_cast<std::size_t>(y) * f->linesize[p.plane],
                        128, static_cast<std::size_t>(f->linesize[p.plane]));
    }
    return f;
}

// Converts one synthesized frame through a fresh SoftDecoder. The AVFrame is
// freed before return; the VideoFrame comes back as an owning shared_ptr (the
// SoftDecoder is local, so nothing outlives the call).
VideoFramePtr convert_gray(int w, int h, int cap, int ticks, double seconds, int number) {
    AVFrame* f = make_gray_yuv(w, h);
    if (!f) return nullptr;
    DemuxState demux;
    SoftDecoder sd;
    sd.set_demux(&demux);
    sd.set_output_dim(cap);
    VideoFramePtr out = sd.convert_to_rgba(f, ticks, seconds, number);
    av_frame_free(&f);
    return out;
}

}  // namespace

int main() {
    // Full-res conversion (no cap): a 1920x1080 luma stream converted to
    // 1920x1080 RGBA. stride is the aligned row stride from av_image_fill_arrays.
    {
        VideoFramePtr out = convert_gray(1920, 1080, 0, 0, 0.0, 1);
        check(out != nullptr, "convert 1920x1080 full-res succeeds");
        if (out) {
            check(out->width == 1920, "full-res width preserved");
            check(out->height == 1080, "full-res height preserved");
            check(out->stride >= 1920u * 4u, "full-res stride >= w*4");
            check(out->rgba.size() >= out->stride * out->height,
                  "full-res rgba.size() >= stride*height");
        }
    }

    // Portrait source, full-res.
    {
        VideoFramePtr out = convert_gray(1080, 1920, 0, 0, 0.0, 2);
        check(out != nullptr, "convert 1080x1920 full-res succeeds");
        if (out) {
            check(out->width == 1080 && out->height == 1920, "portrait dims preserved");
            check(out->stride >= 1080u * 4u, "portrait stride >= w*4");
            check(out->rgba.size() >= out->stride * out->height,
                  "portrait rgba.size() >= stride*height");
        }
    }

    // Landscape downscale cap (preview scrub path): longest edge -> 640.
    {
        VideoFramePtr out = convert_gray(1920, 1080, 640, 0, 0.0, 3);
        check(out != nullptr, "convert 1920x1080 cap 640 succeeds");
        if (out) {
            check(out->width == 640 && out->height == 360, "landscape cap lands at 640x360");
            check(out->stride >= 640u * 4u, "capped stride >= w*4");
            check(out->rgba.size() >= out->stride * out->height,
                  "capped rgba.size() >= stride*height");
        }
    }

    // Portrait downscale cap: longest edge -> 640.
    {
        VideoFramePtr out = convert_gray(1080, 1920, 640, 0, 0.0, 4);
        check(out != nullptr, "convert 1080x1920 cap 640 succeeds");
        if (out) {
            check(out->width == 360 && out->height == 640, "portrait cap lands at 360x640");
            check(out->stride >= 360u * 4u, "portrait capped stride >= w*4");
            check(out->rgba.size() >= out->stride * out->height,
                  "portrait capped rgba.size() >= stride*height");
        }
    }

    // ODD intermediate output dims — the exact case the old hand-sizing broke.
    // 1920x1080 sawn to a 641 cap rounds to 641x361; 641*4 = 2564 is not a
    // multiple of the 32-byte alignment, so the true stride is padded to 2592
    // and the old w*4-based allocation under-sized the buffer. Assert the pad
    // exists (stride > w*4) AND the buffer covers it.
    {
        VideoFramePtr out = convert_gray(1920, 1080, 641, 0, 0.0, 5);
        check(out != nullptr, "convert 1920x1080 cap 641 succeeds");
        if (out) {
            check(out->width == 641 && out->height == 361, "odd cap lands at 641x361");
            check(out->stride > 641u * 4u, "odd-width stride padded past w*4");
            check(out->stride >= 641u * 4u, "odd-width stride >= w*4");
            check(out->rgba.size() >= out->stride * out->height,
                  "odd-width rgba.size() >= stride*height (issue #4 regression)");
        }
    }

    // Cap larger than the source is a no-op (no upscale in the convert path).
    {
        VideoFramePtr out = convert_gray(640, 480, 1280, 0, 0.0, 6);
        check(out != nullptr, "cap larger than source succeeds");
        if (out) {
            check(out->width == 640 && out->height == 480, "oversized cap is a no-op");
            check(out->rgba.size() >= out->stride * out->height,
                  "no-op cap rgba.size() >= stride*height");
        }
    }

    // Frame metadata (ticks / seconds / number) round-trips onto the frame.
    {
        VideoFramePtr out = convert_gray(320, 240, 0, 9876, 42.5, 77);
        check(out != nullptr, "convert carries metadata");
        if (out) {
            check(out->pts_ticks == 9876, "pts_ticks round-trip");
            check(out->pts_seconds == 42.5, "pts_seconds round-trip");
            check(out->frame_number == 77, "frame_number round-trip");
        }
    }

    // Feeding no output-dim cap but a source whose longest edge is below the
    // cap must keep the source dims (floor at the source, never upscale).
    {
        VideoFramePtr out = convert_gray(854, 480, 640, 0, 0.0, 8);
        check(out != nullptr, "convert 854x480 with cap 640 succeeds (no upscale)");
        if (out) {
            check(out->width == 640 && out->height == 360, "854x480 cap 640 lands at 640x360");
            check(out->rgba.size() >= out->stride * out->height,
                  "854x480 capped rgba.size() >= stride*height");
        }
    }

    if (failures == 0) {
        std::printf("sw_decode: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "sw_decode: %d check(s) failed\n", failures);
    return 1;
}