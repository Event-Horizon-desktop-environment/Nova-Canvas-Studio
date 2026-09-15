// Shared helpers for the device-bound CUDA/NVENC encode tests (cuda_enc_test.cpp,
// cuda_enc_bench.cpp). Mirrors vaapi_test_common.hpp so both families read
// alike: same project generators (single-clip + editorial-content with burn-in
// title and edge fades), same export-timing / decode-verify helpers, but the
// device probe targets the CUDA/NVENC path instead of VAAPI.
//
// These tests are DEVICE-BOUND: they SKIP (exit 2) when the machine has no
// working CUDA *encode* node (no NVIDIA GPU / no CUDA runtime / an FFmpeg build
// without NVENC). The gate for CUDA here is TWO-fold, like VAAPI's:
//   - canvas::core::gpu::cuda_available() — the app's own runtime probe (also
//     the fallback's answer when canvas_core wasn't built with CUDA), and
//   - a REAL NVENC encode through av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA)
//     + frames + h264_nvenc/hevc_nvenc — device-init success is NOT enough,
//     a real encode is (an FFmpeg build can list the encoder while the driver
//     refuses to actually create a session).
//
// The test SOURCE is the same real 1440p60 clip as the VAAPI family
// (CANVAS_TEST_CLIP override; SKIP when absent) so the pipeline exercises
// genuine high-rate footage. Target render is H.265 NVENC 1440p @ 80 Mbps @
// 60 fps. The editorial-content projector (make_feature_project) forces the
// CPU-compositor path via a burn-in title, exactly like the VAAPI tests, so
// both device families gate the same full title->composite->encode chain.

#pragma once

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/gpu/cuda_convert.hpp"
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

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace cuda_test {

// Output/artifact root shared by the two CUDA tests (kept separate from the
// VAAPI family's so a mixed run never overwrites the other family's outputs).
inline std::string art_root() {
    return "/tmp/canvas_cuda_enc";
}

// Honors CANVAS_TEST_CLIP identically to the VAAPI family.
inline std::string default_clip_path() {
    if (const char* p = std::getenv("CANVAS_TEST_CLIP")) return p;
    return "/home/matt/Videos/clips/2026-09-10 14-28-50.mkv";
}

// Render target shared by the tests: H.265 NVENC, 2560x1440, 60 fps,
// 80 Mbps bitrate-driven.
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

// True when the app's own CUDA runtime probe says an accelerator is usable.
// This is ALSO the fallback path when canvas_core was built WITHOUT CUDA (the
// no-cuda inline returns false), so the CUDA tests must always run this first.
inline bool cuda_runtime_ok() {
    return canvas::core::gpu::cuda_available();
}

// True when `enc_name` can actually encode on CUDA device ordinal `ordinal`
// ("" = FFmpeg-default device index 0). The gate is a real 2-frame encode +
// flush producing packets — device-init success alone is NOT enough, exactly
// like the VAAPI family's rule.
inline bool encode_probe(const std::string& ordinal, const char* enc_name) {
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_CUDA,
                               ordinal.empty() ? nullptr : ordinal.c_str(), nullptr, 0) < 0)
        return false;
    if (!dev) return false;

    AVBufferRef* frames_ref = av_hwframe_ctx_alloc(dev);
    bool ok = false;
    if (frames_ref) {
        AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
        fc->format = AV_PIX_FMT_CUDA;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = 320;
        fc->height = 240;
        fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames_ref) == 0) {
            const AVCodec* codec = avcodec_find_encoder_by_name(enc_name);
            if (codec) {
                AVCodecContext* ctx = avcodec_alloc_context3(codec);
                if (ctx) {
                    ctx->width = 320;
                    ctx->height = 240;
                    ctx->time_base = AVRational{1, 30};
                    ctx->framerate = AVRational{30, 1};
                    ctx->pix_fmt = AV_PIX_FMT_CUDA;
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

// Finds a working CUDA *encode* device. Returns its ordinal (e.g. "0"), or ""
// (SKIP condition). Honors CANVAS_CUDA_DEVICE to force a specific ordinal on
// multi-GPU boxes. Requires the app's CUDA runtime probe first; a machine with
// no CUDA runtime (or a non-CUDA canvas_core build) always reports "".
inline std::string pick_cuda_encode_device() {
    if (!cuda_runtime_ok()) return "";
    const char* over = std::getenv("CANVAS_CUDA_DEVICE");
    if (over && *over) return encode_probe(over, "h264_nvenc") ? over : "";
    for (int i = 0; i < 8; ++i) {
        const std::string ord = std::to_string(i);
        for (const char* enc : {"h264_nvenc", "hevc_nvenc"}) {
            if (encode_probe(ord, enc)) return ord;
        }
    }
    return "";
}

// One-shot export timing through the real export_project() path.
struct EncResult {
    bool ok = false;
    std::string err;
    double fps = 0.0;  // steady-state frames/sec (see SteadyFps below)
    double ms = 0.0;   // wall time across the whole export incl. session open
    int64_t bytes = 0;
};

// Steady-state fps from the exporter's per-frame progress samples. Short
// head windows (90 frames) are dominated by the fixed per-export session cost
// — the NVDEC device open alone is ~120 ms on this box (canvas_debug.log
// "[dec] open ... ms=119"), plus NVENC session setup and mux init — so the
// naive frames/wall reads ~3x under the sustained rate (the app's real long
// exports log ~550-935 fps from the very same encode loop). Drop the trailing
// mux-flush plateau (identical progress), then measure over the BACK HALF of
// the remaining samples' wall span, which carries none of the session-open cost.
struct SteadyFps {
    std::vector<std::pair<double, std::chrono::steady_clock::time_point>> s;
    std::int64_t total = 1;
    void add(double p, std::chrono::steady_clock::time_point t) {
        if (s.size() < (std::size_t)1 << 16 && p >= 0.0)
            s.emplace_back(p < 1.0 ? p : 1.0, t);
    }
    double fps() const {
        if (s.size() < 4) return 0.0;
        std::size_t hi = s.size();
        while (hi >= 2 && s[hi - 1].first == s[hi - 2].first) --hi;  // flush plateau
        if (hi < 2) hi = s.size();
        const std::size_t lo = hi / 2;
        const double dt = std::chrono::duration<double>(s[hi - 1].second - s[lo].second).count();
        if (dt <= 0.0) return 0.0;
        return (s[hi - 1].first - s[lo].first) * static_cast<double>(total) / dt;
    }
};

inline EncResult run_export(const canvas::core::Project& proj, const std::string& codec,
                            const std::string& fmt, int w, int h, double fps,
                            int64_t frames, int crf, const std::string& vid_rc_mode,
                            const std::string& preset, int bitrate_kbps,
                            const std::string& extra, const std::string& tag) {
    if (::mkdir(art_root().c_str(), 0755) != 0 && errno != EEXIST) {
        EncResult r{};
        r.err = "cannot create artifact root " + art_root();
        return r;
    }
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

    // Watch the exporter's own progress ticks; one callback per encoded frame.
    // The steady-state fps it derives excludes session open / mux flush, so a
    // short bench window reports the sustained encode rate, not a startup-diluted
    // wall-clock average (docs/nvenc.md numbers are the same steady-state kind).
    SteadyFps watch;
    watch.total = frames;
    canvas::core::ExportControl ctl;
    ctl.on_progress = [&watch](double p, const std::string&) {
        watch.add(p, std::chrono::steady_clock::now());
    };

    const auto t0 = std::chrono::steady_clock::now();
    r.ok = canvas::core::export_project(proj, es, &ctl, &r.err);
    r.ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
               .count();
    if (r.ok) {
        r.fps = watch.fps();
        struct stat sb;
        if (::stat(es.output_path.c_str(), &sb) == 0) r.bytes = static_cast<int64_t>(sb.st_size);
    }
    return r;
}

// Decodes an encoded output back with the software decoder. band_frac_h, when
// > 0, ALSO samples the bottom `band_frac_h` of the frame (the classic subtitle
// band) so a burned-in title's arrival at the encoder is verifiable.
struct DecodeResult {
    bool ok = false;
    int frames = 0;
    int64_t lit_luma = 0;      // sampled luma pixels > 16 (not black), whole frame
    int64_t lit_luma_band = 0; // same count, sampled only in the bottom band
    int64_t band_samples = 0;  // pixels sampled inside the band (0 = band skipped)
};

inline DecodeResult verify_decode(const std::string& path, double band_frac_h = 0.0) {
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

    const int band_y = band_frac_h > 0.0 && dctx->height > 0
                           ? static_cast<int>(dctx->height * (1.0 - band_frac_h))
                           : dctx->height;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) == 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(dctx, pkt) == 0) {
            while (avcodec_receive_frame(dctx, frame) == 0) {
                ++out.frames;
                for (int y = 0; y < frame->height; y += 5) {
                    const uint8_t* row = frame->data[0] +
                                         static_cast<std::size_t>(y) * frame->linesize[0];
                    const bool in_band = y >= band_y;
                    for (int x = 0; x < frame->width; x += 5) {
                        const bool lit = row[x] > 16;
                        if (lit) ++out.lit_luma;
                        if (in_band) {
                            ++out.band_samples;
                            if (lit) ++out.lit_luma_band;
                        }
                    }
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

// Fraction of the subtitle band's sampled pixels that are lit (subtitle-like
// content present). Needs a band_sampled decode (band_frac_h > 0).
inline double band_lit_fraction(const DecodeResult& d) {
    return d.band_samples > 0
               ? static_cast<double>(d.lit_luma_band) / static_cast<double>(d.band_samples)
               : 0.0;
}

// Minimal project carrying the source clip's first `frames` timeline frames.
inline canvas::core::Project make_single_clip_project(const TestClip& clip,
                                                      int64_t frames) {
    canvas::core::Project p;
    p.name = "CudaEnc";
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

// Same project but with editorial CONTENT: a burned-in subtitle (pushed to the
// subtitle band) and edge fades. Exports of it take the CPU-compositor path
// (frame_gpu bails on titles), so a CUDA export of this project exercises the
// full title-rasterise + fade + composite + NVENC chain — the same gate as the
// VAAPI family's feature project.
inline canvas::core::Project make_feature_project(const TestClip& clip,
                                                  int64_t frames) {
    canvas::core::Project p = make_single_clip_project(clip, frames);
    canvas::core::Clip& c = p.sequence.video_tracks[0].clips[0];

    canvas::core::Clip::Title t;
    t.text = "NOVA CANVAS SUBTITLE TEST";
    t.size = 0.055f;
    t.a = 1.0f;
    t.bold = true;
    t.box = true;
    t.box_opacity = 0.55f;
    t.box_pad_x = 16.0f;
    t.box_pad_y = 10.0f;
    t.box_radius = 6.0f;
    c.title = std::move(t);

    c.pos_y = static_cast<double>(clip.height) * 0.32;

    c.transition_in = canvas::core::TransitionType::FadeIn;
    c.transition_in_duration = 12;
    c.transition_out = canvas::core::TransitionType::DipToBlack;
    c.transition_out_duration = 12;
    return p;
}

}  // namespace cuda_test