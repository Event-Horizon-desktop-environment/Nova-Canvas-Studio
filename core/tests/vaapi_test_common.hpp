// Shared helpers for the device-bound VAAPI encode tests (vaapi_enc_test.cpp,
// vaapi_enc_bench.cpp). Header-only so both tests compile the same source
// generator / device-discovery / export-timing / decode-verify logic without a
// shared .cpp.
//
// These tests are DEVICE-BOUND: they SKIP (exit 2) when the machine has no
// working VAAPI *encode* node (e.g. CI without a GPU, or a box where the only
// VAAPI display is a decode-only adapter like the NVIDIA NVDEC VAAPI shim —
// naive `av_hwdevice_ctx_create` success is NOT the gate, a real encode is).
//
// The test SOURCE is a real 1440p60 clip (default the user's
// ~/Videos/clips/2026-09-10 14-28-50.mkv, overridable via CANVAS_TEST_CLIP) so
// the pipeline exercises decode of genuine high-rate footage, not a synthetic
// postage stamp. Tests that need it SKIP when the clip is absent. Target render
// is H.265 VAAPI 1440p @ 80 Mbps @ 60 fps.

#pragma once

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace vaapi_test {

// Output/artifact root shared by the two tests.
inline std::string art_root() {
    return "/tmp/canvas_vaapi_enc";
}

// The fixed 1440p60 source clip photo-real footage is exported from. Honors
// CANVAS_TEST_CLIP so a different box/clip can drive the same tests.
inline std::string default_clip_path() {
    if (const char* p = std::getenv("CANVAS_TEST_CLIP")) return p;
    return "/home/matt/Videos/clips/2026-09-10 14-28-50.mkv";
}

// Render target shared by the tests: H.265 VAAPI, 2560x1440, 60 fps,
// 80 Mbps bitrate-driven. Matches the deliver intent the bench is optimizing.
inline constexpr int kTargetWidth = 2560;
inline constexpr int kTargetHeight = 1440;
inline constexpr double kTargetFps = 60.0;
inline constexpr int kTargetBitrateKbps = 80000;

// A probed source clip the export pipeline can be pointed at.
struct TestClip {
    std::string path;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t total_frames = -1;

    [[nodiscard]] bool valid() const noexcept {
        return !path.empty() && width > 0 && height > 0 && fps > 0.0;
    }
};

// Probes `path`'s first video stream for dims / fps / frame count. Returns an
// invalid TestClip when the file is missing or has no video stream.
inline TestClip probe_clip(const std::string& path) {
    TestClip out;
    out.path = path;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return out; }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fmt); return out; }
    const AVStream* s = fmt->streams[vs];
    const AVCodecParameters* cp = s->codecpar;
    out.width = cp->width;
    out.height = cp->height;
    if (s->avg_frame_rate.num > 0 && s->avg_frame_rate.den > 0)
        out.fps = static_cast<double>(s->avg_frame_rate.num) / s->avg_frame_rate.den;
    else if (s->r_frame_rate.num > 0 && s->r_frame_rate.den > 0)
        out.fps = static_cast<double>(s->r_frame_rate.num) / s->r_frame_rate.den;
    if (const double dur = fmt->duration > 0
                               ? static_cast<double>(fmt->duration) / AV_TIME_BASE
                               : 0.0;
        dur > 0.0 && out.fps > 0.0)
        out.total_frames = static_cast<int64_t>(std::llround(dur * out.fps));
    else if (s->nb_frames > 0)
        out.total_frames = static_cast<int64_t>(s->nb_frames);
    avformat_close_input(&fmt);
    return out;
}

// True when `enc_name` can actually encode on `node` ("" = FFmpeg-default
// VAAPI display). The gate is a real 2-frame encode+flush producing packets —
// device-init success alone is NOT enough: on some boxes the default VAAPI
// display is a decode-only adapter (NVIDIA NVDEC VAAPI shim) and opens fine but
// cannot encode.
inline bool encode_probe(const std::string& node, const char* enc_name) {
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI,
                               node.empty() ? nullptr : node.c_str(), nullptr, 0) < 0)
        return false;
    if (!dev) return false;

    AVBufferRef* frames_ref = av_hwframe_ctx_alloc(dev);
    bool ok = false;
    if (frames_ref) {
        AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = 320;
        fc->height = 240;
        fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames_ref) == 0) {
            const AVCodec* codec = avcodec_find_encoder_by_name(enc_name);
            if (codec) {
                AVCodecContext* ctx = avcodec_alloc_context3(codec);
                if (ctx) {
                    // VCN (radeonsi gfx1037) refuses sub-128px encodes; 320x240
                    // is comfortably inside the 128-4096 hardware window.
                    ctx->width = 320;
                    ctx->height = 240;
                    ctx->time_base = AVRational{1, 30};
                    ctx->framerate = AVRational{30, 1};
                    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
                    ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
                    ctx->gop_size = 8;
                    ctx->max_b_frames = 0;
                    if (avcodec_open2(ctx, codec, nullptr) == 0) {
                        AVFrame* cpu = av_frame_alloc();
                        cpu->format = AV_PIX_FMT_NV12;
                        cpu->width = 320;
                        cpu->height = 240;
                        if (av_frame_get_buffer(cpu, 64) == 0) {
                            for (int i = 0; i < 2; ++i) {
                                AVFrame* hw = av_frame_alloc();
                                if (hw && av_hwframe_get_buffer(frames_ref, hw, 0) == 0) {
                                    if (av_hwframe_transfer_data(hw, cpu, 0) == 0) {
                                        hw->pts = i;
                                        avcodec_send_frame(ctx, hw);
                                    }
                                    av_frame_free(&hw);
                                } else if (hw) {
                                    av_frame_free(&hw);
                                }
                            }
                            avcodec_send_frame(ctx, nullptr);
                            AVPacket* pkt = av_packet_alloc();
                            int got = 0;
                            while (avcodec_receive_packet(ctx, pkt) == 0) {
                                ++got;
                                av_packet_unref(pkt);
                            }
                            ok = got > 0;
                            av_packet_free(&pkt);
                        }
                        av_frame_unref(cpu);
                        av_frame_free(&cpu);
                    }
                    avcodec_free_context(&ctx);
                }
            }
        }
        av_buffer_unref(&frames_ref);
    }
    av_buffer_unref(&dev);
    return ok;
}

// Finds a working VAAPI *encode* node. Returns:
//   "/dev/dri/renderD128"  — a render node that encodes h264_vaapi
//   "<default>"            — the FFmpeg-default VAAPI display encodes
//   ""                     — nothing encodes (SKIP condition)
// Honors CANVAS_VAAPI_DEVICE to force a specific node (useful on multi-GPU
// boxes where scan order picks the decode-only adapter).
inline std::string pick_vaapi_encode_node() {
    const char* over = std::getenv("CANVAS_VAAPI_DEVICE");
    if (over && *over) {
        return encode_probe(over, "h264_vaapi") ? over : "";
    }
    for (int i = 0; i < 8; ++i) {
        const std::string node = "/dev/dri/renderD" + std::to_string(128 + i);
        struct stat sb;
        if (::stat(node.c_str(), &sb) != 0) continue;
        if (encode_probe(node, "h264_vaapi")) return node;
    }
    return encode_probe("", "h264_vaapi") ? "<default>" : "";
}

// Maps the sentinel returned by pick_vaapi_encode_node to the device argument
// the exporter should receive ("<default>" -> "" -> null device).
inline std::string device_arg_for(const std::string& node) {
    return node == "<default>" ? std::string{} : node;
}

// One-shot export timing through the real export_project() path.
struct EncResult {
    bool ok = false;
    std::string err;
    double fps = 0.0;     // frames/sec measured across export_project()
    double ms = 0.0;
    int64_t bytes = 0;
};

inline EncResult run_export(const canvas::core::Project& proj,
                            const std::string& codec, const std::string& fmt,
                            int w, int h, double fps, int64_t frames, int crf,
                            const std::string& vid_rc_mode, const std::string& preset,
                            int bitrate_kbps, const std::string& extra,
                            const std::string& tag) {
    EncResult r;
    canvas::core::ExportSettings es;
    es.output_path = art_root() + "/out_" + tag + ".mp4";
    es.format = fmt;
    es.video_codec = codec;
    es.audio_codec = "";
    es.width = w;
    es.height = h;
    es.fps = fps;
    es.duration_frames = frames;
    es.crf = crf;
    es.vid_rc_mode = vid_rc_mode;
    es.preset = preset;
    es.video_bitrate_kbps = bitrate_kbps;
    if (bitrate_kbps > 0) es.video_max_bitrate_kbps = bitrate_kbps * 3 / 2;
    es.extra = extra;

    const auto t0 = std::chrono::steady_clock::now();
    r.ok = canvas::core::export_project(proj, es, nullptr, &r.err);
    r.ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
               .count();
    if (r.ok) {
        r.fps = r.ms > 0.0 ? static_cast<double>(frames) / (r.ms / 1000.0) : 0.0;
        struct stat sb;
        if (::stat(es.output_path.c_str(), &sb) == 0) r.bytes = static_cast<int64_t>(sb.st_size);
    }
    return r;
}

// Decodes an encoded output back with the software decoder and reports the
// decoded frame count + a luma-sample "does it carry content" gauge.
struct DecodeResult {
    bool ok = false;
    int frames = 0;
    int64_t lit_luma = 0;  // number of sampled luma pixels > 16 (not black)
};

inline DecodeResult verify_decode(const std::string& path) {
    DecodeResult out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return out; }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fmt); return out; }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmt); return out; }
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    if (!dctx) { avformat_close_input(&fmt); return out; }
    if (avcodec_parameters_to_context(dctx, fmt->streams[vs]->codecpar) < 0) {
        avcodec_free_context(&dctx); avformat_close_input(&fmt); return out;
    }
    if (avcodec_open2(dctx, dec, nullptr) < 0) {
        avcodec_free_context(&dctx); avformat_close_input(&fmt); return out;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) == 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(dctx, pkt) == 0) {
            while (avcodec_receive_frame(dctx, frame) == 0) {
                ++out.frames;
                for (int y = 0; y < frame->height; y += 5) {
                    const uint8_t* row = frame->data[0] +
                                         static_cast<std::size_t>(y) * frame->linesize[0];
                    for (int x = 0; x < frame->width; x += 5)
                        if (row[x] > 16) ++out.lit_luma;
                }
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dctx, nullptr);
    while (avcodec_receive_frame(dctx, frame) == 0) ++out.frames;
    av_packet_unref(pkt);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dctx);
    avformat_close_input(&fmt);
    out.ok = out.frames > 0;
    return out;
}

// Minimal project carrying the source clip's first `frames` timeline frames.
// The clip's own fps/dims come from the probe, so the timeline framerate
// matches the media for a frame-accurate decode.
inline canvas::core::Project make_single_clip_project(const TestClip& clip,
                                                      int64_t frames) {
    canvas::core::Project p;
    p.name = "VaapiEnc";
    p.sequence.fps = clip.fps;
    canvas::core::MediaEntry m;
    m.id = 0;
    m.path = clip.path;
    m.fps = clip.fps;
    m.width = clip.width;
    m.height = clip.height;
    m.total_frames = clip.total_frames >= 0 ? clip.total_frames : frames;
    p.media.push_back(m);
    canvas::core::Track v;
    v.kind = canvas::core::Track::Kind::Video;
    v.name = "V1";
    p.sequence.video_tracks.push_back(std::move(v));
    canvas::core::Clip clip_ent;
    clip_ent.media = 0;
    clip_ent.name = "A";
    clip_ent.tl_in = 0;
    clip_ent.src_in = 0;
    clip_ent.src_out = frames;
    canvas::core::place_clip(p.sequence, canvas::core::Track::Kind::Video, 0,
                             clip_ent, canvas::core::Placement::Overwrite);
    return p;
}

}  // namespace vaapi_test