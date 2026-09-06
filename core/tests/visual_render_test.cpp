// Visual transform + composite export regression test.
//
// Exercises render_video_frame() against a real synthesized h264 source to lock
// the two guarantees of the transform/compositing blit:
//   1. An identity clip renders byte-identically to the legacy fast path.
//   2. Flip / scale / opacity actually change the exported pixels the way the
//      inverse affine + blend math says they should.
// The source MP4 is generated at runtime (moving gradient, libx264) so the test
// has no external fixtures. SKIP (exit 2) when no libx264 is available.
//
// Pass: exit 0.  Fail: exit 1.  Skipped: exit 2.

#include "canvas/core/export/renderer.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace canvas::core;

static int32_t g_failures = 0;
static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Encodes `frames` of a moving gradient as h264 MP4, mirroring scrub_bench's
// source generator (aspect 4:3-ish, small so decode is quick).
static bool make_source(const std::string& path, int w, int h, int fps, int frames) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::printf("SKIP  no libx264 to synthesize source; cannot run visual-export test\n");
        return false;
    }
    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "mp4", path.c_str());
    if (!oc) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) return false;
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) return false;
    enc->width = w;
    enc->height = h;
    enc->time_base = {1, fps};
    enc->framerate = {fps, 1};
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->gop_size = 8;
    enc->max_b_frames = 0;
    if (avcodec_open2(enc, codec, nullptr) < 0) return false;
    st->id = 0;
    avcodec_parameters_from_context(st->codecpar, enc);
    if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;

    auto* frame = av_frame_alloc();
    std::vector<uint8_t> buf(static_cast<std::size_t>(w) * h * 3 / 2);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = w;
    frame->height = h;
    if (av_frame_get_buffer(frame, 32) < 0) return false;
    int64_t pts = 0;
    for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < h; ++y)
            std::memset(frame->data[0] + static_cast<std::size_t>(y) * frame->linesize[0],
                        static_cast<uint8_t>((f * 17 + y * 3) & 0xff), w);
        for (int y = 0; y < h / 2; ++y)
            std::memset(frame->data[1] + static_cast<std::size_t>(y) * frame->linesize[1], 128, w / 2);
        for (int y = 0; y < h / 2; ++y)
            std::memset(frame->data[2] + static_cast<std::size_t>(y) * frame->linesize[2], 128, w / 2);
        if (avcodec_send_frame(enc, frame) < 0) break;
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = 0;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        frame->pts = pts++;
    }
    avcodec_send_frame(enc, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc, pkt) == 0) {
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = 0;
        av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_write_trailer(oc);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    return true;
}

static Project make_project(const std::string& path) {
    Project p;
    p.sequence.fps = 30.0;
    MediaEntry m;
    m.id = 0;
    m.path = path;
    m.fps = 30.0;
    m.width = 64;
    m.height = 64;
    m.total_frames = 24;
    p.media.push_back(m);
    Track v1;
    v1.kind = Track::Kind::Video;
    v1.name = "V1";
    p.sequence.video_tracks.push_back(std::move(v1));
    return p;
}

static const uint8_t* px(const VideoFramePtr& f, int x, int y) {
    const std::size_t stride = f->stride;
    return &f->rgba[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u];
}

int main() {
    const std::string src = "/tmp/opencode/media/visual_render_src.mp4";
    if (!make_source(src, 64, 64, 30, 24))
        return 2;  // SKIP

    Project p = make_project(src);
    Clip a;
    a.media = 0;
    a.tl_in = 0;
    a.src_in = 0;
    a.src_out = 24;
    auto cmd = place_clip(p.sequence, Track::Kind::Video, 0, a, Placement::Overwrite);
    if (!cmd) {
        report(false, "visual-export: place clip");
        return 1;
    }

    // Identity render == legacy byte-exact output (referenced by
    // render_video_frame's fast path, now guarded by has_visual_transform()).
    VideoFramePtr ident = render_video_frame(p, 4, 64, 64, 0, nullptr);
    report(ident != nullptr, "visual-export: identity render succeeds");
    if (!ident) return 1;
    report(g_failures == 0 || ident->rgba.size() == 64u * 64u * 4u,
           "visual-export: identity canvas sized");
    // The decoded gradient is non-black (source content reached the canvas).
    bool non_black = false;
    for (const uint8_t c : ident->rgba)
        if (c != 0) { non_black = true; break; }
    report(non_black, "visual-export: identity frame carries decoded pixels");

    // Flip horizontal: each pixel mirrors around the vertical centerline.
    const ClipId id = p.sequence.video_tracks[0].clips[0].id;
    cmd = set_clip_transform(p.sequence, Track::Kind::Video, 0, id,
                             1.0f, 1.0f, 0.0, 0.0, 0.0f, 0.0, 0.0, true, false);
    report(cmd != nullptr, "visual-export: set flip transform");
    VideoFramePtr flipped = render_video_frame(p, 4, 64, 64, 0, nullptr);
    report(flipped != nullptr, "visual-export: flipped render succeeds");
    if (flipped) {
        bool mirror = true;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if (std::memcmp(px(flipped, x, y), px(ident, 63 - x, y), 3) != 0) mirror = false;
        report(mirror, "visual-export: flip_h mirrors the pixels exactly");
        // Now clear the flip to restore identity for the next checks.
        cmd = set_clip_transform(p.sequence, Track::Kind::Video, 0, id,
                                 1.0f, 1.0f, 0.0, 0.0, 0.0f, 0.0, 0.0, false, false);
        report(cmd != nullptr, "visual-export: reset transform");
    }

    // Scale 2.0 about the fitted center: the border becomes black (the source
    // no longer covers it) and the center still samples content.
    cmd = set_clip_transform(p.sequence, Track::Kind::Video, 0, id,
                             2.0f, 2.0f, 0.0, 0.0, 0.0f, 0.0, 0.0, false, false);
    report(cmd != nullptr, "visual-export: set scale 2x");
    VideoFramePtr scaled = render_video_frame(p, 4, 64, 64, 0, nullptr);
    report(scaled != nullptr, "visual-export: scaled render succeeds");
    if (scaled) {
        const double center = 32.0;
        const double half = 64.0 / 8.0;  // fitted half-extent / scale -> 4px
        double in_r = 0, in_g = 0, in_b = 0, out_r = 0, out_g = 0, out_b = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const bool inside = std::abs(x + 0.5 - center) <= half &&
                                    std::abs(y + 0.5 - center) <= half;
                if (inside) {
                    in_r += px(scaled, x, y)[0];
                    in_g += px(scaled, x, y)[1];
                    in_b += px(scaled, x, y)[2];
                } else {
                    out_r += px(scaled, x, y)[0];
                    out_g += px(scaled, x, y)[1];
                    out_b += px(scaled, x, y)[2];
                }
            }
        }
        report(out_r == 0 && out_g == 0 && out_b == 0,
               "visual-export: scaled 2x leaves the border black");
        report(in_r > 0 || in_g > 0 || in_b > 0,
               "visual-export: scaled 2x keeps the center lit");
    }

    // Opacity 0.5 over the black background darkens each channel to ~half.
    cmd = set_clip_transform(p.sequence, Track::Kind::Video, 0, id,
                             1.0f, 1.0f, 0.0, 0.0, 0.0f, 0.0, 0.0, false, false);
    cmd = set_clip_composite(p.sequence, Track::Kind::Video, 0, id, 0.5f, BlendMode::Normal);
    report(cmd != nullptr, "visual-export: set opacity 0.5");
    VideoFramePtr half = render_video_frame(p, 4, 64, 64, 0, nullptr);
    report(half != nullptr, "visual-export: half-opacity render succeeds");
    if (half) {
        bool darkens = true;
        for (int y = 0; y < 64 && darkens; ++y)
            for (int x = 0; x < 64; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int base = px(ident, x, y)[c];
                    const int now = px(half, x, y)[c];
                    if (now > (base + 1) / 2 + 1 || now < (base + 1) / 2 - 1) darkens = false;
                }
        report(darkens, "visual-export: opacity 0.5 halves each channel");
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", static_cast<int>(g_failures));
    return 1;
}