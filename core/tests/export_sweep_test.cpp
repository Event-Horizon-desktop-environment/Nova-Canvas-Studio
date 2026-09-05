// Export regression sweep. Verifies the exporter can render+encode a real
// timeline into every *valid* codec/container combination that this machine's
// FFmpeg build actually provides (using the CPU encoders that ship with the
// build). Combinations that the container rejects by design (e.g. WebM+H.264,
// MP4+ProRes) are skipped, not counted as failures. Invalid–but–expected combos
// are the GUI restriction's job and are listed in the test as skipped.
//
// The source media is encoded programmatically at runtime (a tiny h264 MP4) so
// the test has no dependency on external fixtures or a system ffmpeg binary.

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/export/exporter.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

using namespace canvas::core;

static int g_failures = 0;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Encodes `frames` of a moving gradient as h264 in an MP4 container at `path`.
static bool make_source(const std::string& path, int w, int h, int fps, int frames) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) { std::printf("SKIP  no libx264 to synthesize source; cannot run export sweep\n"); return false; }
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

    int y_size = w * h, uv_size = (w / 2) * (h / 2);
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

static const char* kOutDir = "/tmp/canvas_export_sweep";

static Project make_project(const std::string& src_path) {
    Project p;
    p.name = "ExportSweep";
    p.sequence.fps = 30.0;
    MediaEntry m; m.id = 0; m.path = src_path; m.fps = 30.0; m.width = 128; m.height = 96;
    m.total_frames = 12; p.media.push_back(m);
    Track v; v.kind = Track::Kind::Video; v.name = "V1";
    p.sequence.video_tracks.push_back(std::move(v));
    Clip clip; clip.media = 0; clip.name = "A"; clip.tl_in = 0; clip.src_in = 0; clip.src_out = 12;
    place_clip(p.sequence, Track::Kind::Video, 0, clip, Placement::Overwrite);
    return p;
}

static bool file_ok(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz > 64;
}

// Returns whether `codec` is a valid video codec for `fmt`. This is the set of
// combinations the GUI exposes (and the muxer actually accepts); anything else
// is container-invalid by construction and is skipped rather than failed.
static bool is_valid_combo(const std::string& fmt, const std::string& codec) {
    const bool h264 = codec.find("264") != std::string::npos;
    const bool h265 = codec.find("265") != std::string::npos ||
                      codec.find("hevc") != std::string::npos;
    const bool av1 = codec.find("av1") != std::string::npos;
    if (fmt == "mp4" || fmt == "matroska") return h264 || h265 || av1;
    if (fmt == "webm") return av1;
    if (fmt == "mxf_op1a" || fmt == "mxf") return h264 || h265;
    if (fmt == "mpeg") return h264;  // MPEG-2 container can't be AV1/H.265
    return false;
}

int main() {
    std::string src = std::string(kOutDir) + "/src.mp4";
    std::system(("mkdir -p " + std::string(kOutDir)).c_str());
    if (!make_source(src, 128, 96, 30, 12)) return 2;

    const std::vector<std::string> formats{"mp4", "matroska", "mxf_op1a", "mpeg", "webm"};
    // Derive the video codec set from the app's own query (list_video_codecs),
    // the same list the Deliver page exposes, restricted to the broad CPU
    // encoders of interest. A hardcoded list would sweep encoders the linked
    // FFmpeg doesn't actually provide (distro builds ship codecs separately)
    // and spuriously fail. Device-dependent encoders (v4l2m2m/vulkan/etc.) are
    // filtered out: they report as software but still require a device node,
    // so they'd spuriously fail here.
    std::vector<std::string> vcodecs;
    for (const CodecInfo& ci : list_video_codecs()) {
        if (ci.hw) continue;
        const std::string& n = ci.name;
        if (n == "libx264" || n == "libx265" ||
            n == "libsvtav1" || n == "libaom-av1")
            vcodecs.push_back(n);
    }

    std::vector<std::string> acodecs;
    for (const CodecInfo& ci : list_audio_codecs())
        if (ci.name == "aac" || ci.name == "mp3" || ci.name == "libmp3lame") acodecs.push_back(ci.name);

    bool any = false;
    for (const std::string& fmt : formats) {
        for (const std::string& vc : vcodecs) {
            const std::string label = fmt + "/" + vc;
            if (!is_valid_combo(fmt, vc)) { std::printf("SKIP  %s (invalid container combo)\n", label.c_str()); continue; }
            const std::string out = std::string(kOutDir) + "/out_" + fmt + "_" + vc + ".mkv";
            ExportSettings es;
            es.output_path = out;
            es.format = fmt;
            es.video_codec = vc;
            es.audio_codec = "";
            es.width = 128; es.height = 96; es.fps = 30.0; es.duration_frames = 12;
            es.crf = 24; es.preset = "ultrafast";
            std::string err;
            bool ok = export_project(make_project(src), es, nullptr, &err) && file_ok(out);
            report(ok, (label + " " + (ok ? "" : err)).c_str());
            if (ok) any = true;
        }
        // Audio is only checked in containers that accept a common codec (AAC).
        if (fmt != "mp4" && fmt != "matroska") continue;
        if (!acodecs.empty() && !vcodecs.empty()) {
            const std::string& vc = vcodecs[0];
            const std::string out = std::string(kOutDir) + "/aud_" + fmt + ".mkv";
            ExportSettings es;
            es.output_path = out;
            es.format = fmt;
            es.video_codec = vc;
            es.audio_codec = acodecs[0];
            es.width = 128; es.height = 96; es.fps = 30.0; es.duration_frames = 12;
            es.crf = 24; es.preset = "ultrafast";
            std::string err;
            bool ok = export_project(make_project(src), es, nullptr, &err) && file_ok(out);
            report(ok, ("aud " + fmt + "/" + vc + "+" + acodecs[0] + " " + (ok ? "" : err)).c_str());
        }
    }

    if (!any) { std::printf("NO valid combos executed; cannot verify exporter\n"); return 2; }
    if (g_failures == 0) { std::printf("ALL EXPORT SWEEP TESTS PASSED\n"); return 0; }
    std::printf("EXPORT SWEEP FAILURES: %d\n", g_failures);
    return 1;
}
