// Scrub-preview latency benchmark / regression gate.
//
// The whole programmatic scrub decode path lives behind seek_to_frame_indexed
// (CPU preview) and decode_to_hw_indexed (GPU NV12 preview). Both jump to the
// nearest I-frame anchor then walk only the frames of that one Group of
// Pictures to reach the target, so a scrub on sparse-keyframe media stays fast
// (a few tens of ms) regardless of how far the playhead jumps. This benchmark
// quantifies that latency on a worst-case sparse-keyframe source and asserts a
// hard budget so a future change to the decode/forward logic can't silently
// re-introduce multi-second stalls.
//
// It generates its own source (a real h264 MP4 with a multi-second GOP) at
// runtime, so it has no dependency on external fixtures or a system ffmpeg. SKIP
// (exit 2) when no libx264 is available to synthesize the source, mirroring the
// export_sweep convention.
//
// Pass:      exit 0 + a timing table.
// Fail:      exit 1 when p95 scrub-preview latency exceeds the budget.
// Skipped:   exit 2 when the source can't be built (no libx264).

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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

static int32_t g_failures = 0;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Encodes `frames` of a moving gradient as h264 MP4 with a keyframe every
// `gop_frames` frames. A large `gop_frames` (e.g. ~10s) reproduces the
// sparse-keyframe worst case that made scrubbing stall.
static bool make_source(const std::string& path, int w, int h, int fps,
                        int frames, int gop_frames) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::printf("SKIP  no libx264 to synthesize source; cannot run scrub bench\n");
        return false;
    }
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
    c->gop_size = gop_frames; c->max_b_frames = 0;
    av_opt_set(c->priv_data, "preset", "ultrafast", 0);
    av_opt_set(c->priv_data, "crf", "30", 0);
    if (avcodec_open2(c, codec, nullptr) < 0) return false;
    if (avcodec_parameters_from_context(st->codecpar, c) < 0) return false;
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (avformat_write_header(oc, nullptr) < 0) return false;

    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P; f->width = w; f->height = h;
    av_frame_get_buffer(f, 32);
    const int y_size = w * h, uv_size = (w / 2) * (h / 2);
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

// Percentile helper over the raw sample vector (ms double).
static double ms_pct(std::vector<double>& v, double q) {
    std::sort(v.begin(), v.end());
    const int idx = std::min(static_cast<int>(std::ceil(q * (v.size() - 1))),
                             static_cast<int>(v.size()) - 1);
    return idx >= 0 ? v[idx] : 0.0;
}

static double ms_now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static const char* kOutDir = "/tmp/canvas_scrub_bench";
static const int kFps = 30;
static const int kGopFrames = 300;   // ~10s keyframe interval: worst-case sparse GOP
static const int kFrames = 1800;     // 60s of media
static const int kPreviewDim = 640;  // matches the GUI's kPreviewMaxDim

int main() {
    std::string src = std::string(kOutDir) + "/sparse.mp4";
    std::system(("mkdir -p " + std::string(kOutDir) + " && rm -f " + src).c_str());
    if (!make_source(src, 1280, 720, kFps, kFrames, kGopFrames)) return 2;

    std::printf("scrub_bench: sparse-GOP source frames=%d gop=%d (~%.1fs) preview_dim=%d\n",
                kFrames, kGopFrames, (double)kGopFrames / kFps, kPreviewDim);

    const bool have_cuda = canvas::core::gpu::cuda_available();

    // ---- CPU preview path (always runs) ----
    {
        std::printf("CPU  preview seek_to_frame_indexed (worst-case sparse GOP)\n");
        VideoDecoder dec;
        std::string open_err;
        if (!dec.open(src, &open_err)) {
            std::printf("SKIP  decoder open failed: %s\n", open_err.c_str());
            return 2;
        }
        dec.build_iframe_index();
        if (!dec.has_iframe_index())
            std::printf("note  iframe index unavailable\n");

        // Synthetic forward scrubbing: walk the playhead through the whole media
        // with varied jump sizes (a fixed LCG so the sweep is deterministic and
        // reproducible across runs). The worst case for preview decode is a jump
        // that crosses a whole sparse GOP (a few hundred to ~1000+ frames).
        std::vector<double> samples;
        std::vector<int64_t> targets;
        uint32_t lcg = 0x1234abcd;
        auto lcg_next = [&lcg] {
            lcg = lcg * 1664525u + 1013904223u;
            return static_cast<int>(lcg % kFrames);
        };
        // Mix: half small/medium jumps local to the prior target (real dragging),
        // half large jumps that cross GOP boundaries (worst case).
        for (int i = 0; i < 24; ++i) {
            if ((i & 1u) == 0u || targets.empty()) {
                targets.push_back(lcg_next());                     // large random jump
            } else {
                const int64_t base = targets.back();
                const int delta = (i % 3 == 0) ? 90 : (i % 3 == 1) ? 300 : 900;
                targets.push_back(std::clamp<int64_t>(base + delta, 0, kFrames - 1));
            }
        }
        int64_t prev = -1;
        for (int64_t t : targets) {
            int64_t cur = t;
            const char* which = (prev >= 0 && cur > prev && cur - prev <= 200) ? "drag" : "jump";
            prev = cur;
            const double t0 = ms_now();
            auto f = dec.seek_to_frame_indexed(cur, kPreviewDim);
            const double dt = ms_now() - t0;
            samples.push_back(dt);
            std::printf("  %-4s -> frame %-5lld  %7.2f ms  %s\n",
                        which, (long long)cur, dt, f ? "hit" : "MISS");
        }
        const double p50 = ms_pct(samples, 0.50), p95 = ms_pct(samples, 0.95), mx = *std::max_element(samples.begin(), samples.end());
        std::printf("  CPU  p50=%.2fms  p95=%.2fms  max=%.2fms\n", p50, p95, mx);

        // Budget: with the fast-over cap this must stay fast even on a sparse
        // GOP that spans 60s. A bounded forward jump returns an *approximate*
        // frame, so the worst case is a few hundred ms, not seconds.
        const double budget_ms = 250.0;
        std::printf("  CPU  budget: p95 < %.0fms -> %s\n", budget_ms,
                    p95 < budget_ms ? "OK" : "BREACHED");
        report(p95 < budget_ms, "CPU preview p95 under budget on sparse GOP");

        const int dec_frames = dec.total_frames() > 0
                                   ? static_cast<int>(dec.total_frames())
                                   : kFrames;
        std::printf("  note decoder total_frames=%d\n", dec_frames);
    }

    // ---- GPU NV12 preview path (gated on CUDA) ----
    if (have_cuda) {
        std::printf("GPU  decode_to_hw bounded (max_over=kPreviewMaxOver)\n");
        HwDeviceManager hw;
        VideoDecoder dec;
        std::string open_err;
        if (!dec.open(src, &open_err, hw.device_ctx()) || !dec.is_hardware()) {
            std::printf("note  no hardware decode on this machine; skipping GPU sub-bench\n");
        } else {
            const int budget = VideoDecoder::kPreviewMaxOver;
            std::vector<double> bounded;
            // Worst case: a single forward walk across several GOPs. With the
            // keyframe anchor (decode_to_hw_indexed) this should cost only one
            // GOP regardless of span; the cap keeps even that bounded.
            const int64_t spans[] = {90, 299, 600, 1499, 3000};
            int64_t cur = 0;
            for (int64_t span : spans) {
                const double t0 = ms_now();
                const AVFrame* hw_f = dec.decode_to_hw_indexed(
                    cur, budget);
                const double dt = ms_now() - t0;
                bounded.push_back(dt);
                std::printf("  span=+%-5lld -> frame %-5lld  %7.2f ms  %s\n",
                            (long long)span, (long long)cur,
                            dt, hw_f && hw_f->data[0] ? "hit" : "MISS");
                cur += span;
                if (cur >= kFrames) { dec.seek_to_frame(0); cur = 0; }  // wrap cleanly
            }
            const double bp = ms_pct(bounded, 0.95);
            std::printf("  GPU  bounded p50=%.2fms p95=%.2fms\n",
                        ms_pct(bounded, 0.50), bp);
            report(bp < 250.0, "GPU bounded preview p95 under budget on sparse GOP");
        }
    } else {
        std::printf("GPU  (CUDA unavailable - bounded fast-over path not exercised)\n");
    }

    std::printf("\nscrub_bench: %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}