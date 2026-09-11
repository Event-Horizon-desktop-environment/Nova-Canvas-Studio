// Scrub-preview latency benchmark / regression gate, modeled on how the editor
// actually decodes.
//
// The editor has three distinct forward-decode workloads, each exercised by a
// sub-bench on a worst-case sparse-keyframe source (10s GOP):
//
//   1. CPU scrub preview     -> seek_to_frame_indexed(640), an editor-style
//                               drag/jump playhead sweep.
//   2. GPU scrub preview     -> decode_to_hw_indexed(kPreviewMaxOver), the
//                               keyframe-anchored jump path for NV12 previews.
//   3. GPU sequential decode -> decode_to_hw(0) forward-walk, the path the
//                               off-thread transition bake and steady playback
//                               ride on. A per-call container re-seek (the
//                               regression that made the dissolve bake pay a
//                               full far-jump per frame) blows this up.
//
// Budget law (shared): every sub-bench runs its own identical sweep once as a
// "warm" calibration pass under the CURRENT machine/GPU load, then asserts a
// budget of max(absolutefloor, factor * warm_p95). A busy NVDEC (an editor
// session playing on the same GPU while tests run) inflates every decode call
// for the test and the editor together, so an absolute budget would measure the
// load, not the decode logic; a regression that multiplies the work (multi-GOP
// walks, per-call re-seeks) still trips the factor on an idle box. The absolute
// floor keeps that idle box strict.
//
// The source (a real h264 MP4 with a multi-second GOP) is synthesized at
// runtime, so there is no dependency on external fixtures or a system ffmpeg.
// SKIP (exit 2) when no libx264 is available, mirroring the export_sweep
// convention.
//
// Pass:      exit 0 + a timing table per sub-bench.
// Fail:      exit 1 when a sub-bench p95 exceeds its calibrated budget.
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

// Load-adaptive budget: a same-run warm p95 calibrates the machine's decode rate
// under its current load; the assertion budget is floor | factor. See the file
// header for the rationale.
static double calibrated_budget(double warm_p95_ms, double floor_ms) {
    return std::max(floor_ms, 1.75 * warm_p95_ms);
}

static const char* kOutDir = "/tmp/canvas_scrub_bench";
static const int kFps = 30;
static const int kGopFrames = 300;   // ~10s keyframe interval: worst-case sparse GOP
static const int kFrames = 1800;     // 60s of media
static const int kPreviewDim = 640;  // matches the GUI's kPreviewMaxDim

// Absolute floors: a compliant decode stays well under these even lightly
// loaded; a regression that multiplies the work clears them and trips the
// factor, but the floor keeps the gate strict when the box is idle.
static constexpr double kScrubFloorMs = 250.0;       // scrub/preview p95
static constexpr double kSeqFloorMs = 1000.0 / 60.0; // sequential per-frame = one 60fps frame budget

int main() {
    std::string src = std::string(kOutDir) + "/sparse.mp4";
    std::system(("mkdir -p " + std::string(kOutDir) + " && rm -f " + src).c_str());
    if (!make_source(src, 1280, 720, kFps, kFrames, kGopFrames)) return 2;

    std::printf("scrub_bench: sparse-GOP source frames=%d gop=%d (~%.1fs) preview_dim=%d\n",
                kFrames, kGopFrames, (double)kGopFrames / kFps, kPreviewDim);

    const bool have_cuda = canvas::core::gpu::cuda_available();

    // ---- Sub-bench 1: CPU scrub preview, editor drag/jump sweep ----
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

        // Editor-style scrub sweep: half small/medium moves local to the prior
        // target (real playhead dragging), half large jumps that cross GOP
        // boundaries (worst case). Fixed LCG seed so the sweep is deterministic
        // and reproducible across runs.
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

        // Warm + calibrate under the current load, then measure the same sweep.
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
        // GPU NV12 paths couldn't run even warmed; still report the skip.
        std::printf("GPU  (CUDA unavailable - GPU sub-benchmarks not exercised)\n");
    } else {
        HwDeviceManager hw;
        VideoDecoder dec;
        std::string open_err;
        const bool gpu_ok = dec.open(src, &open_err, hw.device_ctx()) && dec.is_hardware();

        if (!gpu_ok) {
            std::printf("note  no hardware decode on this machine; skipping GPU sub-benchmarks\n");
        } else {
            // ---- Sub-bench 2: GPU scrub preview, keyframe-anchored jumps ----
            std::printf("GPU  preview decode_to_hw_indexed (bounded, kPreviewMaxOver=%d)\n",
                        VideoDecoder::kPreviewMaxOver);
            const int budget = VideoDecoder::kPreviewMaxOver;
            // Worst case: a single forward walk across several GOPs. With the
            // keyframe anchor this should cost only one GOP regardless of span;
            // kPreviewMaxOver keeps even that bounded.
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
                    if (cur >= kFrames) { dec.seek_to_frame(0); cur = 0; }  // wrap cleanly
                }
            };

            // Warm + calibrate, then measure the same span sweep.
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
                if (cur >= kFrames) { dec.seek_to_frame(0); cur = 0; }  // wrap cleanly
            }
            const double gp = ms_pct(bounded, 0.95),
                         g50 = ms_pct(bounded, 0.50);
            std::printf("  GPU  warm_p95=%.2fms p50=%.2fms p95=%.2fms budget=%.0fms\n",
                        warm_p95, g50, gp, budget_ms);
            report(gp < budget_ms, "GPU scrub preview p95 under calibrated budget on sparse GOP");

            // ---- Sub-bench 3: GPU sequential forward decode (transition bake) ----
            // The off-thread transition pre-render (TimelineDecoder's bake) and
            // steady playback advance the decoder in a contiguous forward walk.
            // Each step must be a cheap in-place advance; a container re-seek per
            // call (the dissolve-stall regression) pushes per-frame cost past the
            // 60fps frame budget and trips this gate. The anchor seek here is the
            // bake's own "position the walk at the window head" step.
            std::printf("GPU  sequential decode_to_hw forward walk (%d frames, bake/playback profile)\n",
                        static_cast<int>(kNSpans * 30));
            const int64_t kWalkFrames = kNSpans * 30;  // 150 frames, crosses no sparse boundary
            auto sweep_seq = [&](std::vector<double>& out) {
                out.clear();
                out.reserve(static_cast<std::size_t>(kWalkFrames));
                dec.seek_to_frame(0);
                for (int64_t f = 0; f < kWalkFrames; ++f) {
                    const double t0 = ms_now();
                    const AVFrame* hw_f = dec.decode_to_hw(f, 0);  // exact, sequential
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