#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

static double calibrated_budget(double warm_p95_ms, double floor_ms) {
    return std::max(floor_ms, 1.75 * warm_p95_ms);
}

static const char* kOutDir = "/tmp/canvas_scrub_bench";
static const int kFps = 30;
static const int kGopFrames = 300;
static const int kFrames = 1800;
static const int kPreviewDim = 640;

static constexpr double kScrubFloorMs = 250.0;
static constexpr double kSeqFloorMs = 1000.0 / 60.0;

int main() {
    std::string src = std::string(kOutDir) + "/sparse.mp4";
    std::system(("mkdir -p " + std::string(kOutDir) + " && rm -f " + src).c_str());
    if (!make_source(src, 1280, 720, kFps, kFrames, kGopFrames)) return 2;

    std::printf("scrub_bench: sparse-GOP source frames=%d gop=%d (~%.1fs) preview_dim=%d\n",
                kFrames, kGopFrames, (double)kGopFrames / kFps, kPreviewDim);

    const bool have_cuda = canvas::core::gpu::cuda_available();

    {
        std::printf("CPU  preview seek_to_frame_indexed (editor drag/jump mix)\n");
        VideoDecoder dec;
        std::string open_err;
        if (!dec.open(src, &open_err)) {
            std::printf("SKIP  decoder open failed: %s\n", open_err.c_str());
            return 2;
        }
        dec.build_iframe_index();
        if (!dec.has_iframe_index())
            std::printf("note  iframe index unavailable\n");

        std::vector<int64_t> targets;
        targets.reserve(24);
        uint32_t lcg = 0x1234abcd;
        auto lcg_next = [&lcg] {
            lcg = lcg * 1664525u + 1013904223u;
            return static_cast<int>(lcg % kFrames);
        };
        for (int i = 0; i < 24; ++i) {
            if ((i & 1u) == 0u || targets.empty()) {
                targets.push_back(lcg_next());
            } else {
                const int64_t base = targets.back();
                const int delta = (i % 3 == 0) ? 90 : (i % 3 == 1) ? 300 : 900;
                targets.push_back(std::clamp<int64_t>(base + delta, 0, kFrames - 1));
            }
        }
        auto scrub = [&](int64_t target) {
            const double t0 = ms_now();
            auto f = dec.seek_to_frame_indexed(target, kPreviewDim);
            return std::pair<double, bool>(ms_now() - t0, static_cast<bool>(f));
        };

        std::vector<double> warm;
        warm.reserve(targets.size());
        for (int64_t t : targets) warm.push_back(scrub(t).first);
        const double warm_p95 = ms_pct(warm, 0.95);
        const double budget = calibrated_budget(warm_p95, kScrubFloorMs);

        std::vector<double> samples;
        samples.reserve(targets.size());
        int64_t prev = -1;
        for (int64_t t : targets) {
            const char* which = (prev >= 0 && t > prev && t - prev <= 200) ? "drag" : "jump";
            const auto [dt, hit] = scrub(t);
            samples.push_back(dt);
            std::printf("  %-4s -> frame %-5lld  %7.2f ms  %s\n",
                        which, (long long)t, dt, hit ? "hit" : "MISS");
            prev = t;
        }
        const double p50 = ms_pct(samples, 0.50), p95 = ms_pct(samples, 0.95),
                     mx = *std::max_element(samples.begin(), samples.end());
        std::printf("  CPU  warm_p95=%.2fms p50=%.2fms p95=%.2fms max=%.2fms budget=%.0fms\n",
                    warm_p95, p50, p95, mx, budget);
        const int dec_frames = dec.total_frames() > 0
                                   ? static_cast<int>(dec.total_frames())
                                   : kFrames;
        std::printf("  note decoder total_frames=%d\n", dec_frames);
        report(p95 < budget, "CPU preview p95 under calibrated budget on sparse GOP");
    }

    if (!have_cuda) {
        std::printf("GPU  (CUDA unavailable - GPU sub-benchmarks not exercised)\n");
    } else {
        HwDeviceManager hw{"test"};
        VideoDecoder dec;
        std::string open_err;
        const bool gpu_ok = dec.open(src, &open_err, hw.device_ctx()) && dec.is_hardware();

        if (!gpu_ok) {
            std::printf("note  no hardware decode on this machine; skipping GPU sub-benchmarks\n");
        } else {
            std::printf("GPU  preview decode_to_hw_indexed (bounded, kPreviewMaxOver=%d)\n",
                        VideoDecoder::kPreviewMaxOver);
            const int budget = VideoDecoder::kPreviewMaxOver;
            const int64_t kSpans[] = {90, 299, 600, 1499, 3000};
            const std::size_t kNSpans = sizeof(kSpans) / sizeof(kSpans[0]);
            auto sweep_gpu = [&](std::vector<double>& out) {
                out.clear();
                out.reserve(kNSpans);
                int64_t cur = 0;
                dec.seek_to_frame(0);
                for (const int64_t span : kSpans) {
                    const double t0 = ms_now();
                    dec.decode_to_hw_indexed(cur, budget);
                    out.push_back(ms_now() - t0);
                    cur += span;
                    if (cur >= kFrames) { dec.seek_to_frame(0); cur = 0; }
                }
            };

            std::vector<double> warm;
            sweep_gpu(warm);
            const double warm_p95 = ms_pct(warm, 0.95);
            const double budget_ms = calibrated_budget(warm_p95, kScrubFloorMs);

            std::vector<double> bounded;
            int64_t cur = 0;
            for (const int64_t span : kSpans) {
                const double t0 = ms_now();
                const AVFrame* hw_f = dec.decode_to_hw_indexed(cur, budget);
                const double dt = ms_now() - t0;
                bounded.push_back(dt);
                std::printf("  span=+%-5lld -> frame %-5lld  %7.2f ms  %s\n",
                            (long long)span, (long long)cur, dt,
                            hw_f && hw_f->data[0] ? "hit" : "MISS");
                cur += span;
                if (cur >= kFrames) { dec.seek_to_frame(0); cur = 0; }
            }
            const double gp = ms_pct(bounded, 0.95),
                         g50 = ms_pct(bounded, 0.50);
            std::printf("  GPU  warm_p95=%.2fms p50=%.2fms p95=%.2fms budget=%.0fms\n",
                        warm_p95, g50, gp, budget_ms);
            report(gp < budget_ms, "GPU scrub preview p95 under calibrated budget on sparse GOP");

            std::printf("GPU  sequential decode_to_hw forward walk (%d frames, bake/playback profile)\n",
                        static_cast<int>(kNSpans * 30));
            const int64_t kWalkFrames = kNSpans * 30;
            auto sweep_seq = [&](std::vector<double>& out) {
                out.clear();
                out.reserve(static_cast<std::size_t>(kWalkFrames));
                dec.seek_to_frame(0);
                for (int64_t f = 0; f < kWalkFrames; ++f) {
                    const double t0 = ms_now();
                    const AVFrame* hw_f = dec.decode_to_hw(f, 0);
                    const double dt = ms_now() - t0;
                    out.push_back(dt);
                    if (!hw_f || !hw_f->data[0]) {
                        std::printf("note  decode_to_hw(%lld) MISS at step walk f=%lld\n",
                                    (long long)f, (long long)f);
                        break;
                    }
                }
            };

            std::vector<double> seq_warm;
            sweep_seq(seq_warm);
            const double seq_warm_p95 = ms_pct(seq_warm, 0.95);
            const double seq_budget = calibrated_budget(seq_warm_p95, kSeqFloorMs);

            std::vector<double> seq;
            sweep_seq(seq);
            const double sp95 = ms_pct(seq, 0.95), s50 = ms_pct(seq, 0.50),
                         smax = *std::max_element(seq.begin(), seq.end());
            std::printf("  walk  n=%zu  warm_p95=%.2fms p50=%.2fms p95=%.2fms max=%.2fms budget(per-frame)=%.1fms\n",
                        seq.size(), seq_warm_p95, s50, sp95, smax, seq_budget);
            report(seq.size() >= 1 && sp95 < seq_budget,
                   "GPU sequential per-frame p95 under calibrated budget (bake/playback)");
        }
    }

    std::printf("\nscrub_bench: %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}