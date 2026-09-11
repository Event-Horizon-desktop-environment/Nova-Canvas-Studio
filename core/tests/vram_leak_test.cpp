// Device-memory (VRAM) leak regression test.
//
// Playback holds ~30fps of NVDEC surfaces + host NV12 downloads in flight; a
// slot or NVDEC pool that fails to return device memory grows VRAM
// unboundedly until cudaMalloc starts failing with "out of memory" (observed
// as 1/sec host-nv12-staging nulls that dropped the playhead to the CPU GOP
// re-walk, and as the full 16GB card being consumed during a session).
//
// Reproduces the two allocation patterns playback actually runs, while
// sampling cudaMemGetInfo between/during phases:
//   Phase A  steady playback shape: 600 native-res NV12 decodes+downloads.
//   Phase B  reopen churn: repeated slot close/open (project/jump resets).
// A leak shows up as the free byte count draining monotonically across the
// steady-state phase (the FFmpeg surface pool plateaus after warm-up).
//
// SKIP (exit 2) when no libx264 (no source) or no CUDA hw decode. Fail exit 1
// when steady-state free VRAM drops more than kLeakToleranceMiB.

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"

#include <algorithm>
#include <chrono>
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

#ifdef CANVAS_HAVE_CUDA
#include <cuda_runtime.h>
#endif

using namespace canvas::core;

static int32_t g_failures = 0;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Encodes `frames` of a moving gradient as h264 MP4 with a keyframe every
// `gop_frames` frames. 2K1440p mirrors the real media this editor ships.
static bool make_source(const std::string& path, int w, int h, int fps,
                        int frames, int gop_frames) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::printf("SKIP  no libx264 to synthesize source\n");
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

#ifdef CANVAS_HAVE_CUDA
static long long vram_free_mib() {
    std::size_t free_b = 0, total_b = 0;
    const cudaError_t e = cudaMemGetInfo(&free_b, &total_b);
    if (e != cudaSuccess) return -1;
    return static_cast<long long>(free_b / (1024 * 1024));
}
#else
static long long vram_free_mib() { return -1; }
#endif

static const int kFps = 30;
static const int kFrames = 700;
static const int kGopFrames = 250;
static const int kW = 2560;
static const int kH = 1440;
// Steady-state VRAM drain (warm baseline -> post-phase) above which we call
// it a leak. The FFmpeg NVDEC surface pool plateaus during warm-up; a genuine
// leak keeps draining every frame and clears this in <5s of playback.
static constexpr long long kLeakToleranceMiB = 256;

int main() {
#ifdef CANVAS_HAVE_CUDA
    const std::string out_dir = "/tmp/canvas_vram_leak";
    const std::string src = out_dir + "/src.mp4";
    std::system(("mkdir -p " + out_dir + " && rm -f " + src).c_str());
    std::printf("vram_leak: 2K1440p source frames=%d gop=%d\n", kFrames, kGopFrames);
    if (!make_source(src, kW, kH, kFps, kFrames, kGopFrames)) return 2;

    HwDeviceManager hw{"vram-leak-test"};
    {
        VideoDecoder dec;
        std::string open_err;
        if (!dec.open(src, &open_err, hw.device_ctx()) || !dec.is_hardware()) {
            std::printf("note  no hardware decode on this machine; skipping\n");
            return 2;
        }
        dec.build_iframe_index();
        const long long total = ([] {
            std::size_t f = 0, t = 0;
            return cudaMemGetInfo(&f, &t) == cudaSuccess
                       ? static_cast<long long>(t / (1024 * 1024))
                       : -1LL;
        })();
        std::printf("GPU decode active, VRAM total=%lld MiB\n", total);

        // Warm: a few decodes so the NVDEC surface pool (and any one-time
        // context/pool allocation) reaches its plateau before we measure.
        for (int i = 0; i < 40; ++i)
            dec.decode_to_hw_indexed(i, 0);
        const long long warm_free = vram_free_mib();
        std::printf("warm  free_vram=%lld MiB\n", warm_free);

        // Phase A -- steady playback shape: 600 native-res NV12 decodes +
        // host downloads, sampled every 40 frames.
        long long min_free = warm_free;
        for (int i = 41; i <= 600; ++i) {
            const AVFrame* hw = dec.decode_to_hw_indexed(i, 0);
            if (!hw || !hw->data[0]) {
                std::printf("FAIL  decode null at frame %d\n", i);
                ++g_failures;
                break;
            }
            std::vector<uint8_t> y, uv;
            if (!gpu::convert_nv12_resize_to_host(
                    reinterpret_cast<const uint8_t*>(hw->data[0]),
                    reinterpret_cast<const uint8_t*>(hw->data[1]),
                    hw->width, hw->height,
                    static_cast<std::size_t>(hw->linesize[0]),
                    static_cast<std::size_t>(hw->linesize[1]),
                    hw->width, hw->height, hw->width, hw->height, 0, 0, &y, &uv)) {
                std::printf("FAIL  host nv12 convert failed at frame %d (%s)\n", i,
                            gpu::cuda_last_error_string());
                ++g_failures;
                break;
            }
            if (i % 40 == 0) {
                const long long fr = vram_free_mib();
                min_free = std::min(min_free, fr);
                std::printf("  A   frame=%-4d free_vram=%lld MiB\n", i, fr);
            }
        }
        const long long a_free = vram_free_mib();
        report(a_free >= warm_free - kLeakToleranceMiB,
               "Phase A steady playback does not drain VRAM");
        std::printf("  A   warm=%lld end=%lld min=%lld\n", warm_free, a_free, min_free);

        // Phase B -- reopen churn: close/reopen is what playhead re-anchors and
        // project/lookahead resets do; a leaked device context shows here.
        const long long b0 = vram_free_mib();
        for (int r = 0; r < 10; ++r) {
            dec.close();
            std::string err2;
            if (!dec.open(src, &err2, hw.device_ctx()) || !dec.is_hardware()) {
                std::printf("FAIL  reopen %d failed\n", r);
                ++g_failures;
                break;
            }
            dec.build_iframe_index();
            dec.decode_to_hw_indexed(0, 0);
        }
        const long long b_free = vram_free_mib();
        report(b_free >= b0 - kLeakToleranceMiB,
               "Phase B slot reopen churn does not drain VRAM");
        std::printf("  B   start=%lld end=%lld\n", b0, b_free);
    }

    if (g_failures == 0) std::printf("vram_leak: ok\n");
    return g_failures == 0 ? 0 : 1;
#else
    std::printf("SKIP  CUDA not compiled in; no VRAM to measure\n");
    return 2;
#endif
}