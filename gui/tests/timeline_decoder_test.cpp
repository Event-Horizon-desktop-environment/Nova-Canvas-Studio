// Qt-free unit test for TimelineDecoder (the extracted decode/assemble front-end).
//
// Encodes a tiny h264 MP4 at runtime (no external fixtures or system ffmpeg),
// builds a one-clip Project, and verifies the public surface:
//   * decode(): full-res dims, repeat = cache hit (same frame), max_dim cap
//   * frame()/preview(): RenderFrame assembly returns pixels
//   * disabled clip -> all-zero black fallback frame
//   * media_rate_at(): media fps vs fallback
//   * invalidate()/close(): slot teardown
// Links ONLY canvas_core + timeline_decoder.cpp (no Qt) — the "unbreakable seam" that
// keeps the extracted headless module display-free.

#include "features/playback/sync_constants.hpp"
#include "features/playback/timeline_decoder.hpp"

#include "canvas/core/colorsci/wheels.hpp"
#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/model.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

using namespace canvas::core;

static int g_failures = 0;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Encodes `frames` of a moving gradient as h264 in an MP4 container at `path`
// (same synthesis the export sweep uses). Returns false when libx264 is missing.
static bool make_source(const std::string& path, int w, int h, int fps, int frames) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) return false;
    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "mp4", path.c_str());
    if (!oc) return false;

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) return false;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c) return false;
    c->width = w; c->height = h;
    c->time_base = AVRational{1, fps};
    st->time_base = c->time_base;
    c->framerate = AVRational{fps, 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->gop_size = 12; c->max_b_frames = 0;
    av_opt_set(c->priv_data, "preset", "ultrafast", 0);
    av_opt_set(c->priv_data, "crf", "30", 0);
    if (avcodec_open2(c, codec, nullptr) < 0) return false;
    if (avcodec_parameters_from_context(st->codecpar, c) < 0) return false;

    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (avformat_write_header(oc, nullptr) < 0) return false;

    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P; f->width = w; f->height = h;
    av_frame_get_buffer(f, 32);

    for (int n = 0; n < frames; ++n) {
        av_frame_make_writable(f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                f->data[0][y * f->linesize[0] + x] = (uint8_t)((x + y + n * 16) & 0xff);
        for (int y = 0; y < h / 2; ++y) {
            for (int x = 0; x < w / 2; ++x) {
                f->data[1][y * f->linesize[1] + x] = 128;
                f->data[2][y * f->linesize[2] + x] = 128;
            }
        }
        f->pts = n;
        avcodec_send_frame(c, f);
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(c, pkt) == 0) {
            av_packet_rescale_ts(pkt, c->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&f);
    avcodec_send_frame(c, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(c, pkt) == 0) {
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;
        av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_write_trailer(oc);
    avcodec_free_context(&c);
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_free_context(oc);
    return true;
}

int main() {
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    constexpr int kFps = 30;
    constexpr int kFrames = 36;
    const std::string path = "/tmp/canvas_td_test.mp4";

    if (!make_source(path, kWidth, kHeight, kFps, kFrames)) {
        std::printf("SKIP  no libx264 to synthesize source; cannot run TimelineDecoder test\n");
        return 0;
    }

    // One video track, one clip covering the whole media, src window aligned.
    Project project;
    project.name = "decoder-test";
    project.sequence.fps = kFps;
    project.sequence.next_clip_id = 1;

    MediaEntry media;
    media.id = 1;
    media.path = path;
    media.fps = kFps;
    media.width = kWidth;
    media.height = kHeight;
    media.total_frames = kFrames;
    project.media.push_back(media);

    Track track;
    track.kind = Track::Kind::Video;
    Clip clip;
    clip.id = project.sequence.next_clip_id++;
    clip.media = media.id;
    clip.tl_in = 0;
    clip.tl_out = kFrames;
    clip.src_in = 0;
    clip.src_out = kFrames;
    clip.name = "T";
    track.clips.push_back(clip);
    project.sequence.video_tracks.push_back(std::move(track));

    canvas::gui::TimelineDecoder decoder;
    decoder.add_media(media);

    // Full-res decode through the slot cache.
    auto f0 = decoder.decode(project, clip, 5);
    report(f0 && f0->width == kWidth && f0->height == kHeight,
           "decode(project, clip, frame) full-res dims");
    if (!f0 || f0->width != kWidth || f0->height != kHeight) {
        std::fprintf(stderr, "      got %dx%d path=%s\n", f0 ? f0->width : -1,
                     f0 ? f0->height : -1, path.c_str());
    }

    // Repeating the same frame must be a cache hit that returns the SAME frame
    // (FrameCache identity), not a fresh decode.
    auto f1 = decoder.decode(project, clip, 5);
    report(f0.get() == f1.get(), "decode repeat is a cache hit (same frame ptr)");

    // Reduced decode caps the longest edge at max_dim.
    auto fp = decoder.decode(project, clip, 10, 40);
    report(fp && fp->width <= 40 && fp->height <= 40, "decode max_dim caps longest edge");

    // Decode past the media's last frame must CLAMP to the last addressable
    // source frame instead of walking the GOP chain to EOF (the 78s stall when a
    // music region extended past a short clip; also a far-forward seek into an
    // ultra-sparse GOP). Assert the returned frame number is within the source
    // and the call actually returns a frame within the bounded walk.
    auto fEnd = decoder.decode(project, clip, 35000);
    report(fEnd != nullptr && fEnd->frame_number >= 0 && fEnd->frame_number < kFrames,
           "decode past media end clamps to last source frame (no infinite walk)");
    auto fEnd2 = decoder.decode(project, clip, kFrames + 5000, 40);
    report(fEnd2 != nullptr && fEnd2->frame_number >= 0 && fEnd2->frame_number < kFrames,
           "preview decode past media end clamps to last source frame");

    // Full-res and preview timeline assembly return pixels (RGBA or GPU NV12).
    auto rf = decoder.frame(project, 5);
    report(rf && (rf->a || rf->nv12), "frame() assembles pixels at seq_frame");
    auto rp = decoder.preview(project, 5, 40);
    report(rp && (rp->a || rp->nv12), "preview() assembles pixels at seq_frame");

    // Phase 6 live graded preview + Phase LUT: a clip owning a grade tree is
    // baked to a 3D LUT attached to the RenderFrame (the viewer's NV12 shader
    // samples it) and the CPU RGBA path applies the same LUT, so preview ==
    // export by construction. Graded clips therefore ride the NV12 fast path on
    // GPU machines; the small CPU `a` is still attached so the scopes show the
    // graded signal. On software-only machines the graded RGBA path delivers the
    // pixels with no LUT carried (the frame holds no NV12 planes then).
    {
        Project pgraded = project;
        Clip graded = clip;
        canvas::core::grade_graph::GradeGraph g;
        const int lgg = g.add_node(canvas::core::grade_graph::NodeKind::kCorrector);
        g.node(lgg).correct_mode = canvas::core::grade_graph::CorrectMode::kLgg;
        canvas::core::colorsci::LGG bright;
        bright.lift_master = 0.4f;  // additive lift across all channels
        g.node(lgg).lgg = bright;
        const int gout = g.add_node(canvas::core::grade_graph::NodeKind::kOutput);
        g.add_rgb_edge(lgg, gout);
        graded.grade = g;
        pgraded.sequence.video_tracks[0].clips[0] = graded;

        auto gf = decoder.frame(pgraded, 5);
        report(gf && gf->a && gf->a->width == kWidth && gf->a->height == kHeight,
               "graded clip frame() carries full-res graded pixels");
        report(gf && (!gf->nv12 || (gf->grade && gf->grade->valid())),
               "graded clip frame() NV12 path attaches a valid grade LUT");

        auto raw = decoder.decode(project, clip, 5);
        bool differs = false;
        if (gf && gf->a && raw && gf->a->rgba.size() == raw->rgba.size()) {
            for (std::size_t i = 0; i < raw->rgba.size(); ++i) {
                if (raw->rgba[i] != gf->a->rgba[i]) { differs = true; break; }
            }
        }
        report(differs, "graded clip frame() pixels differ from raw media");

        auto gp = decoder.preview(pgraded, 5, 40);
        report(gp && gp->a && (!gp->nv12 || (gp->grade && gp->grade->valid())),
               "graded clip preview() carries graded pixels (LUT on NV12 path)");
    }

    // Disabled clip → black fallback frame of the media dims, all-zero pixels.
    Clip disabled = clip;
    disabled.enabled = false;
    auto fb = decoder.decode(project, disabled, 5);
    bool black = fb != nullptr && fb->width == kWidth && fb->height == kHeight;
    if (black) {
        for (const auto byte : fb->rgba) {
            if (byte != 0) { black = false; break; }
        }
    }
    report(black, "disabled clip -> all-zero black fallback");

    // Media pacing: covered frame returns media fps, uncovered falls back.
    const double rate = decoder.media_rate_at(project, 5, 25.0);
    report(rate > kFps - 0.5 && rate < kFps + 0.5, "media_rate_at returns media fps");
    const double fallback = decoder.media_rate_at(project, 500, 25.0);
    report(fallback == 25.0, "media_rate_at falls back outside sequence");

    // Mixed frame-rate mapping: a 60fps source on a 30fps timeline strides TWO
    // source frames per seq-frame (time-based), so the clip plays at its
    // intended speed instead of half-speed slow motion. Media fps == seq fps
    // keeps the old 1:1 mapping (covered above at 30/30).
    const std::string path60 = "/tmp/canvas_td_test_60.mp4";
    if (make_source(path60, kWidth, kHeight, 60, 48)) {
        Project p60;
        p60.name = "decoder-test-60";
        p60.sequence.fps = 30;
        p60.sequence.next_clip_id = 1;
        MediaEntry m60;
        m60.id = 2;
        m60.path = path60;
        m60.fps = 60;
        m60.width = kWidth;
        m60.height = kHeight;
        m60.total_frames = 48;
        p60.media.push_back(m60);
        Track t60;
        t60.kind = Track::Kind::Video;
        Clip c60;
        c60.id = p60.sequence.next_clip_id++;
        c60.media = m60.id;
        c60.tl_in = 0;
        c60.tl_out = 24;
        c60.src_in = 0;
        c60.src_out = 48;
        t60.clips.push_back(c60);
        p60.sequence.video_tracks.push_back(std::move(t60));
        decoder.add_media(m60);

        auto head = decoder.decode(p60, c60, 0);
        report(head && head->frame_number == 0, "60fps-in-30fps: seq 0 maps to source 0");
        auto two = decoder.decode(p60, c60, 10);
        report(two && two->frame_number == 20, "60fps-in-30fps: seq 10 strides to source 20");
        auto preview60 = decoder.preview(p60, 10, 40);
        report(preview60 && (preview60->a || preview60->nv12),
               "60fps-in-30fps: preview assembles at the mapped source frame");
        decoder.invalidate(m60.id);
    }

    // invalidation drops the slot + preview entries; decode then yields null.
    decoder.invalidate(media.id);
    report(!decoder.is_loaded(media.id), "invalidate -> is_loaded false");
    report(decoder.decode(project, clip, 5) == nullptr, "invalidate -> decode null");

    // Re-arm and tear down.
    decoder.add_media(media);
    report(decoder.is_loaded(media.id), "add_media re-loads after invalidate");
    decoder.close();
    report(decoder.decode(project, clip, 5) == nullptr, "close() -> decode null");
    report(!decoder.is_loaded(media.id), "close() -> is_loaded false");

    std::printf("%s\n", g_failures == 0 ? "timeline_decoder_test: ALL PASS" : "timeline_decoder_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}