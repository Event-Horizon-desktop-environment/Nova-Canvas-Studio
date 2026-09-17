#include "canvas/core/export/exporter.hpp"

#include "canvas/core/export/renderer.hpp"
#include "canvas/core/export/vaapi_encode.hpp"
#include "canvas/core/export/qsv_encode.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/timeline/title.hpp"
#include "canvas/core/util/log.hpp"

#include <cctype>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace canvas::core {

namespace {

constexpr std::chrono::milliseconds kPreviewPushInterval{33};

void apply_codec_extra(AVCodecContext* ctx, const std::string& extra) {
    std::istringstream iss(extra);
    std::string line;
    while (std::getline(iss, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        k.erase(0, k.find_first_not_of(" \t"));
        k.erase(k.find_last_not_of(" \t") + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t") + 1);
        if (!k.empty()) av_opt_set(ctx->priv_data, k.c_str(), v.c_str(), 0);
    }
}

AVPixelFormat pick_video_fmt(const AVCodec* codec, bool* uses_hw) {
    *uses_hw = false;
    const enum AVPixelFormat* fmts = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, (const void**)&fmts, &n) == 0 && fmts) {
        AVPixelFormat hw = AV_PIX_FMT_NONE;
        AVPixelFormat sw = AV_PIX_FMT_NONE;
        for (int i = 0; i < n; ++i) {
            const AVPixelFormat f = fmts[i];
            if (f == AV_PIX_FMT_NONE) continue;
            if (f == AV_PIX_FMT_CUDA || f == AV_PIX_FMT_VAAPI || f == AV_PIX_FMT_QSV ||
                f == AV_PIX_FMT_DRM_PRIME || f == AV_PIX_FMT_D3D11) {
                *uses_hw = true;
                if (hw == AV_PIX_FMT_NONE) hw = f;
            } else if (f == AV_PIX_FMT_NV12 || f == AV_PIX_FMT_YUV420P) {
                if (sw == AV_PIX_FMT_NONE) sw = f;
            }
        }
        if (*uses_hw) return hw;
        if (sw != AV_PIX_FMT_NONE) return sw;
    }
    return AV_PIX_FMT_YUV420P;
}

std::string hw_device_for_codec(const std::string& codec) {
    if (codec.find("vaapi") != std::string::npos) return "vaapi";
    if (codec.find("qsv") != std::string::npos) return "qsv";
    if (codec.find("amf") != std::string::npos) return "amf";
    if (codec.find("nvenc") != std::string::npos) return "cuda";
    if (codec.find("vulkan") != std::string::npos) return "vulkan";
    return "";
}

bool is_hw_codec(const std::string& codec) {
    return !hw_device_for_codec(codec).empty();
}

bool is_hw_pix_fmt(AVPixelFormat f) {
    return f == AV_PIX_FMT_CUDA || f == AV_PIX_FMT_VAAPI || f == AV_PIX_FMT_QSV ||
           f == AV_PIX_FMT_DRM_PRIME || f == AV_PIX_FMT_D3D11;
}

struct GpuGradeLut {
    const grade_graph::GradeLut3D* baked = nullptr;
    void* dev = nullptr;

    bool ensure(const RenderSession::GpuFrameInfo& gfi) {
        if (!gfi.grade || !gfi.grade->valid()) return false;
        if (baked == gfi.grade.get()) return dev != nullptr;
        release();
        dev = canvas::core::gpu::grade_lut_upload(gfi.grade->data.data(), gfi.grade->size);
        baked = dev ? gfi.grade.get() : nullptr;
        if (dev) {
            CANVAS_LOG("render: grade LUT uploaded to device size=%d seq=%llu",
                   gfi.grade->size, (unsigned long long)gfi.grade->change_seq);
        }
        return dev != nullptr;
    }

    gpu::GradeKernelParams params(const RenderSession::GpuFrameInfo& gfi) const {
        gpu::GradeKernelParams p;
        p.lut = static_cast<const float*>(dev);
        p.lut_size = gfi.grade ? gfi.grade->size : 0;
        const gpu::MatrixCoeffs k = gpu::matrix_coeffs(static_cast<gpu::ColorMatrix>(gfi.matrix),
                                                       static_cast<gpu::ColorRange>(gfi.range));
        p.r_cr = k.r_cr;
        p.g_cb = k.g_cb;
        p.g_cr = k.g_cr;
        p.b_cb = k.b_cb;
        p.range = gfi.range;
        return p;
    }

    void release() {
        if (dev) {
            canvas::core::gpu::grade_lut_free(dev);
            dev = nullptr;
        }
        baked = nullptr;
    }
};

struct GpuTitleCache {
    const Clip* clip = nullptr;
    gpu::TitleSpriteGpu spr;

    bool ensure(const Clip* c, int out_w, int out_h) {
        if (!c || !c->has_title() || out_w <= 0 || out_h <= 0) return false;
        if (clip == c) return spr.valid();
        release();
        const std::string font = canvas::core::title::find_font_path_for(c->title.font_family);
        if (font.empty()) return false;
        const title::TitleSprite host =
            canvas::core::title::raster_title_sprite(*c, out_w, out_h, font);
        if (!host.valid()) return false;
        if (!canvas::core::gpu::title_sprite_upload(host.data.data(), host.width, host.height,
                                                    host.ox, host.oy, &spr))
            return false;
        clip = c;
        CANVAS_LOG("render: title sprite uploaded w=%d h=%d ox=%d oy=%d clip=%llu",
               spr.w, spr.h, spr.ox, spr.oy, (unsigned long long)c->id);
        return true;
    }

    void release() {
        canvas::core::gpu::title_sprite_free(&spr);
        clip = nullptr;
    }
};

enum class ExportBackend {
    Software,
    Cuda,
    Vaapi,
};

struct PreviewThrottle {
    std::chrono::steady_clock::time_point last{};
    bool due() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < kPreviewPushInterval) return false;
        last = now;
        return true;
    }
};

struct PumpFrame {
    AVFrame* frame = nullptr;
    void* event = nullptr;
    AVFrame* source = nullptr;
};

class FramePump {
public:
    ExportBackend kind = ExportBackend::Software;
    int out_w = 0, out_h = 0;
    double tl_per_frame = 1.0;
    AVPixelFormat enc_sw_fmt = AV_PIX_FMT_YUV420P;

    ::AVBufferRef* dec_dev = nullptr;
    ::AVBufferRef* hw_frames = nullptr;

    SwsContext* sws = nullptr;
    AVFrame* rgb = nullptr;

    GpuGradeLut grade_lut;
    GpuTitleCache title_cache;
    int64_t judder_prev_src = INT64_MIN;

    AVFrame* va_scan = nullptr;
    AVFrame* va_scaled = nullptr;
    AVFrame* va_out = nullptr;
    SwsContext* va_sws = nullptr;
    int va_sws_src_w = 0, va_sws_src_h = 0, va_sws_dst_w = 0, va_sws_dst_h = 0;

    canvas::core::log::RenderTelemetry* telemetry = nullptr;
    PreviewThrottle* throttle = nullptr;
    std::function<void(const VideoFramePtr&)> push_preview;
    const Project* project = nullptr;
    RenderSession* session = nullptr;
    bool session_ok = false;

    bool create_device_and_pool(const std::string& hw_device, AVPixelFormat hw_pix,
                                int w, int h) {
        out_w = w;
        out_h = h;
        const AVHWDeviceType dt = av_hwdevice_find_type_by_name(hw_device.c_str());
        if (dt == AV_HWDEVICE_TYPE_NONE) return false;
        ::AVBufferRef* dev = nullptr;
        const std::string& gpu_arg =
            (hw_device == canvas::core::HwDeviceManager::preferred_gpu_backend())
                ? canvas::core::HwDeviceManager::preferred_device_arg()
                : std::string{};
        av_hwdevice_ctx_create(&dev, dt, gpu_arg.empty() ? nullptr : gpu_arg.c_str(),
                               nullptr, 0);
        dec_dev = dev;
        if (!dec_dev) return false;

        ::AVBufferRef* fr = av_hwframe_ctx_alloc(dev);
        if (fr) {
            AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(fr->data);
            if (fc) {
                fc->format = hw_pix;
                fc->sw_format = AV_PIX_FMT_NV12;
                fc->width = w;
                fc->height = h;
                fc->initial_pool_size = 12;
            }
            if (av_hwframe_ctx_init(fr) == 0) {
                hw_frames = fr;
                enc_hw_fmt_ = reinterpret_cast<AVHWFramesContext*>(fr->data)->format;
            } else {
                av_buffer_unref(&fr);
            }
        }
        if (!hw_frames) return false;

        if (enc_hw_fmt_ == AV_PIX_FMT_CUDA && canvas::core::gpu::cuda_available())
            kind = ExportBackend::Cuda;
        else if (enc_hw_fmt_ == AV_PIX_FMT_VAAPI)
            kind = ExportBackend::Vaapi;
        else
            kind = ExportBackend::Software;
        if (kind == ExportBackend::Vaapi) {
            va_scan = av_frame_alloc();
            va_scaled = av_frame_alloc();
            va_out = av_frame_alloc();
        }
        return true;
    }

    bool init_converters(int w, int h, AVPixelFormat fmt, bool bt709) {
        out_w = w;
        out_h = h;
        tagged_bt709_ = bt709;
        enc_sw_fmt = fmt;
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        sws = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, fmt,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        const int conv_matrix = bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        const int* conv_coefs = sws_getCoefficients(conv_matrix);
        sws_setColorspaceDetails(sws, conv_coefs, 1, conv_coefs, 0,
                                 0, 1 << 16, 1 << 16);
        if (rgb) av_frame_free(&rgb);
        rgb = av_frame_alloc();
        rgb->format = AV_PIX_FMT_RGBA;
        rgb->width = w;
        rgb->height = h;
        av_frame_get_buffer(rgb, 0);
        return true;
    }

    const char* matrix_name() const {
        return tagged_bt709_ ? "BT.709" : "BT.601";
    }

    PumpFrame produce(int64_t tl, int64_t out_frame) {
        if (kind == ExportBackend::Cuda) return produce_cuda(tl, out_frame);
        if (kind == ExportBackend::Vaapi) return produce_vaapi(tl, out_frame);
        return produce_software(tl, out_frame);
    }

    void after_send() {
        if (kind == ExportBackend::Cuda)
            canvas::core::gpu::convert_nv12_device_sync();
    }
    void account_stalls() {
        if (kind == ExportBackend::Cuda)
            telemetry->note_pool_stalls(canvas::core::gpu::nv12_pool_stalls());
    }

    void teardown() {
        if (kind == ExportBackend::Cuda) {
            grade_lut.release();
            title_cache.release();
        }
        av_frame_free(&rgb);
        if (va_sws) {
            sws_freeContext(va_sws);
            va_sws = nullptr;
        }
        av_frame_free(&va_out);
        av_frame_free(&va_scaled);
        av_frame_free(&va_scan);
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (hw_frames) av_buffer_unref(&hw_frames);
        if (dec_dev) av_buffer_unref(&dec_dev);
    }

private:
    AVPixelFormat enc_hw_fmt_ = AV_PIX_FMT_NONE;
    bool tagged_bt709_ = false;

    VideoFramePtr frame_for(int64_t tl) {
        if (session_ok && session) return session->frame(tl);
        return render_video_frame(*project, tl, out_w, out_h);
    }

    bool composite_rgba(int64_t tl, VideoFramePtr& vf) {
        const auto comp_t0 = std::chrono::steady_clock::now();
        vf = frame_for(tl);
        if (!vf) return false;
        if (throttle && throttle->due() && push_preview) push_preview(vf);
        telemetry->note_cpu(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - comp_t0).count());
        const std::size_t bytes =
            std::min<std::size_t>(vf->rgba.size(),
                                  rgb->linesize[0] * (std::size_t)out_h);
        std::memcpy(rgb->data[0], vf->rgba.data(), bytes);
        return true;
    }

    PumpFrame produce_cuda(int64_t tl, int64_t out_frame) {
        if (session_ok && session) {
            RenderSession::GpuFrameInfo gfi;
            bool gpu_ok = session->frame_gpu(tl, &gfi) && gfi.valid;
            if (gpu_ok && gfi.clip && gfi.clip->has_title())
                gpu_ok = title_cache.ensure(gfi.clip, out_w, out_h);
            if (gpu_ok) {
                telemetry->note_fast();
                const gpu::TitleSpriteGpu* title =
                    (gfi.clip && gfi.clip->has_title()) ? &title_cache.spr : nullptr;
                if (gfi.src_frame >= 0 && tl_per_frame == 1.0) {
                    if (judder_prev_src != INT64_MIN && gfi.src_frame != judder_prev_src + 1) {
                        telemetry->note_stall();
                        fprintf(stderr,
                                "[FRAME-DIAG] tl_frame=%lld src=+%lld (prev src=%lld) delta=%lld\n",
                                (long long)tl, (long long)gfi.src_frame,
                                (long long)judder_prev_src,
                                (long long)(gfi.src_frame - judder_prev_src));
                    }
                    judder_prev_src = gfi.src_frame;
                }
                AVFrame* hw = av_frame_alloc();
                if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                    const uintptr_t yc = reinterpret_cast<uintptr_t>(hw->data[0]);
                    const uintptr_t uvc = reinterpret_cast<uintptr_t>(hw->data[1]);
                    auto _tr0 = std::chrono::steady_clock::now();
                    AVFrame* src_ref = nullptr;
                    if (gfi.source) {
                        src_ref = av_frame_alloc();
                        if (src_ref && av_frame_ref(src_ref, gfi.source) < 0) {
                            av_frame_free(&src_ref);
                            src_ref = nullptr;
                        }
                    }
                    bool resized = src_ref != nullptr;
                    if (resized) {
                        if (gfi.grade && gfi.grade->valid() && grade_lut.ensure(gfi)) {
                            resized = canvas::core::gpu::convert_nv12_grade_resize_async(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy, gfi.fade, grade_lut.params(gfi), title);
                        } else {
                            resized = canvas::core::gpu::convert_nv12_resize_async(
                                reinterpret_cast<const uint8_t*>(gfi.srcY),
                                reinterpret_cast<const uint8_t*>(gfi.srcUV),
                                gfi.srcW, gfi.srcH, gfi.srcYPitch, gfi.srcUVPitch,
                                reinterpret_cast<uint8_t*>(yc),
                                static_cast<std::size_t>(hw->linesize[0]),
                                reinterpret_cast<uint8_t*>(uvc),
                                static_cast<std::size_t>(hw->linesize[1]),
                                gfi.outW, gfi.outH, gfi.dstW, gfi.dstH,
                                gfi.dx, gfi.dy, gfi.fade, title);
                        }
                    }
                    if (resized) {
                        void* ev = nullptr;
                        canvas::core::gpu::convert_nv12_record_event(&ev);
                        auto _tr1 = std::chrono::steady_clock::now();
                        telemetry->note_resize(
                            std::chrono::duration<double, std::milli>(_tr1 - _tr0).count());
                        hw->pts = out_frame;
                        return {hw, ev, src_ref};
                    }
                    if (src_ref) av_frame_unref(src_ref);
                } else {
                    telemetry->note_alloc_miss();
                }
                av_frame_free(&hw);
            }
        }
        VideoFramePtr vf;
        if (!composite_rgba(tl, vf)) return {};
        AVFrame* hw = av_frame_alloc();
        if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(hw->data[0]);
            uint8_t* dY = reinterpret_cast<uint8_t*>(base);
            uint8_t* dUV = reinterpret_cast<uint8_t*>(
                base + static_cast<uintptr_t>(hw->linesize[0]) * static_cast<uintptr_t>(out_h));
            if (canvas::core::gpu::convert_rgba_to_nv12(
                    vf->rgba.data(), vf->width, vf->height,
                    dY, static_cast<std::size_t>(hw->linesize[0]),
                    dUV, static_cast<std::size_t>(hw->linesize[0]),
                    out_w, out_h)) {
                hw->pts = out_frame;
                return {hw, nullptr, nullptr};
            }
            telemetry->note_alloc_miss();
            av_frame_free(&hw);
        } else if (hw) {
            telemetry->note_alloc_miss();
            av_frame_free(&hw);
        }
        return {};
    }

    AVFrame* vaapi_feed_frame(int64_t tl) {
        RenderSession::GpuFrameInfo gfi;
        const auto t0 = std::chrono::steady_clock::now();
        if (!session->frame_gpu(tl, &gfi) || !gfi.valid) return nullptr;
        if (gfi.fade < 1.0f || (gfi.grade && gfi.grade->valid())) return nullptr;
        if (gfi.clip && gfi.clip->has_title()) return nullptr;
        if (gfi.dx < 0 || gfi.dy < 0 || (gfi.dx & 1) || (gfi.dy & 1)) return nullptr;
        if (gfi.dstW <= 0 || gfi.dstH <= 0 || (gfi.dstW & 1) || (gfi.dstH & 1)) return nullptr;
        if (!va_scan || !va_scaled || !va_out) return nullptr;

        av_frame_unref(va_scan);
        va_scan->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(va_scan, gfi.source, 0) < 0) return nullptr;
        const int sw = va_scan->width, sh = va_scan->height;
        const bool identity =
            (gfi.dx == 0 && gfi.dy == 0 && sw == out_w && sh == out_h);
        AVFrame* feed = va_scan;
        if (!identity) {
            if (va_sws_src_w != sw || va_sws_src_h != sh ||
                va_sws_dst_w != gfi.dstW || va_sws_dst_h != gfi.dstH) {
                if (va_sws) sws_freeContext(va_sws);
                va_sws = sws_getContext(sw, sh, AV_PIX_FMT_NV12,
                                        gfi.dstW, gfi.dstH, AV_PIX_FMT_NV12,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                va_sws_src_w = sw; va_sws_src_h = sh;
                va_sws_dst_w = gfi.dstW; va_sws_dst_h = gfi.dstH;
            }
            if (!va_sws) return nullptr;
            av_frame_unref(va_scaled);
            va_scaled->format = AV_PIX_FMT_NV12;
            va_scaled->width = gfi.dstW;
            va_scaled->height = gfi.dstH;
            if (av_frame_get_buffer(va_scaled, 32) < 0) return nullptr;
            const uint8_t* srows[] = {va_scan->data[0], va_scan->data[1], nullptr, nullptr};
            const int slines[] = {va_scan->linesize[0], va_scan->linesize[1], 0, 0};
            sws_scale(va_sws, srows, slines, 0, sh, va_scaled->data, va_scaled->linesize);

            av_frame_unref(va_out);
            va_out->format = AV_PIX_FMT_NV12;
            va_out->width = out_w;
            va_out->height = out_h;
            if (av_frame_get_buffer(va_out, 32) < 0) return nullptr;
            for (int r = 0; r < va_out->height; ++r)
                std::memset(va_out->data[0] + (std::size_t)r * (std::size_t)va_out->linesize[0],
                            16, (std::size_t)va_out->linesize[0]);
            for (int r = 0; r < va_out->height / 2; ++r)
                std::memset(va_out->data[1] + (std::size_t)r * (std::size_t)va_out->linesize[1],
                            128, (std::size_t)va_out->linesize[1]);
            for (int r = 0; r < gfi.dstH; ++r)
                std::memcpy(va_out->data[0] +
                                (std::size_t)(gfi.dy + r) * (std::size_t)va_out->linesize[0] + gfi.dx,
                            va_scaled->data[0] + (std::size_t)r * (std::size_t)va_scaled->linesize[0],
                            (std::size_t)gfi.dstW);
            for (int r = 0; r < gfi.dstH / 2; ++r)
                std::memcpy(va_out->data[1] +
                                (std::size_t)(gfi.dy / 2 + r) * (std::size_t)va_out->linesize[1] + gfi.dx,
                            va_scaled->data[1] + (std::size_t)r * (std::size_t)va_scaled->linesize[1],
                            (std::size_t)gfi.dstW);
            feed = va_out;
        }

        AVFrame* hw = av_frame_alloc();
        if (!hw || av_hwframe_get_buffer(hw_frames, hw, 0) != 0) {
            telemetry->note_alloc_miss();
            av_frame_free(&hw);
            return nullptr;
        }
        if (av_hwframe_transfer_data(hw, feed, 0) != 0) {
            telemetry->note_alloc_miss();
            av_frame_free(&hw);
            return nullptr;
        }
        telemetry->note_fast();
        telemetry->note_resize(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count());
        return hw;
    }

    PumpFrame produce_vaapi(int64_t tl, int64_t out_frame) {
        if (session_ok && session) {
            if (AVFrame* hw = vaapi_feed_frame(tl)) {
                hw->pts = out_frame;
                return {hw, nullptr, nullptr};
            }
        }
        VideoFramePtr vf;
        if (!composite_rgba(tl, vf)) return {};
        AVFrame* input = av_frame_alloc();
        if (!input) return {};
        input->format = enc_sw_fmt;
        input->width = out_w;
        input->height = out_h;
        if (av_frame_get_buffer(input, 0) >= 0) {
            const uint8_t* src[] = {rgb->data[0]};
            int src_lines[] = {rgb->linesize[0]};
            sws_scale(sws, src, src_lines, 0, out_h, input->data, input->linesize);
            input->pts = out_frame;
            AVFrame* hw = av_frame_alloc();
            if (hw && av_hwframe_get_buffer(hw_frames, hw, 0) == 0) {
                if (av_hwframe_transfer_data(hw, input, 0) == 0) {
                    hw->pts = out_frame;
                    av_frame_free(&input);
                    return {hw, nullptr, nullptr};
                }
                av_frame_free(&hw);
            } else if (hw) {
                telemetry->note_alloc_miss();
                av_frame_free(&hw);
            }
        }
        av_frame_free(&input);
        return {};
    }

    PumpFrame produce_software(int64_t tl, int64_t out_frame) {
        VideoFramePtr vf;
        if (!composite_rgba(tl, vf)) return {};
        AVFrame* input = av_frame_alloc();
        if (!input) return {};
        input->format = enc_sw_fmt;
        input->width = out_w;
        input->height = out_h;
        if (av_frame_get_buffer(input, 0) >= 0) {
            const uint8_t* src[] = {rgb->data[0]};
            int src_lines[] = {rgb->linesize[0]};
            sws_scale(sws, src, src_lines, 0, out_h, input->data, input->linesize);
            input->pts = out_frame;
            return {input, nullptr, nullptr};
        }
        av_frame_free(&input);
        return {};
    }
};

}

std::vector<std::string> available_hw_devices() {
    std::vector<std::string> out;
    const char* names[] = {"cuda", "vaapi", "qsv", "amf", nullptr};
    for (int i = 0; names[i]; ++i) {
        const AVHWDeviceType t = av_hwdevice_find_type_by_name(names[i]);
        if (t == AV_HWDEVICE_TYPE_NONE) continue;
        ::AVBufferRef* ref = nullptr;
        if (av_hwdevice_ctx_create(&ref, t, nullptr, nullptr, 0) == 0 && ref) {
            av_buffer_unref(&ref);
            out.push_back(names[i]);
        } else if (ref) {
            av_buffer_unref(&ref);
        }
    }
    return out;
}

std::vector<CodecInfo> list_video_codecs(const std::string& hw_device) {
    std::vector<CodecInfo> out;
    void* iter = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iter)) {
        if (!av_codec_is_encoder(c) || c->type != AVMEDIA_TYPE_VIDEO) continue;
        const std::string name = c->name ? c->name : "";
        if (name.empty() || name == "gif") continue;
        if (name == "libopenh264") continue;

        const bool hw = is_hw_codec(name);
        if (!hw_device.empty()) {
            if (!hw || hw_device_for_codec(name) != hw_device) continue;
        } else {
            if (hw) continue;
        }

        CodecInfo ci;
        ci.name = name;
        ci.long_name = c->long_name ? c->long_name : "";
        ci.media_type = AVMEDIA_TYPE_VIDEO;
        ci.hw = hw;
        ci.hw_device = hw ? hw_device_for_codec(name) : "";
        out.push_back(std::move(ci));
    }
    return out;
}

std::vector<CodecInfo> list_audio_codecs() {
    std::vector<CodecInfo> out;
    void* iter = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iter)) {
        if (!av_codec_is_encoder(c) || c->type != AVMEDIA_TYPE_AUDIO) continue;
        const std::string name = c->name ? c->name : "";
        if (name.empty()) continue;
        CodecInfo ci;
        ci.name = name;
        ci.long_name = c->long_name ? c->long_name : "";
        ci.media_type = AVMEDIA_TYPE_AUDIO;
        out.push_back(std::move(ci));
    }
    return out;
}

std::vector<ContainerInfo> list_containers() {
    std::vector<ContainerInfo> out;
    void* iter = nullptr;
    while (const AVOutputFormat* f = av_muxer_iterate(&iter)) {
        if (!f || !f->extensions || !*f->extensions) continue;
        ContainerInfo ci;
        ci.name = f->name ? f->name : "";
        ci.long_name = f->long_name ? f->long_name : "";
        ci.extensions = f->extensions;
        out.push_back(std::move(ci));
    }
    return out;
}

std::string nv_preset_for(const std::string& codec, const std::string& preset) {
    const std::string p = [&] {
        std::string q = preset;
        for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return q;
    }();

    if (codec.find("svt") != std::string::npos || codec.find("av1") != std::string::npos) {
        if (p == "ultrafast" || p == "superfast") return "13";
        if (p == "veryfast") return "11";
        if (p == "fast") return "9";
        if (p == "faster") return "7";
        if (p == "medium") return "6";
        if (p == "slow") return "4";
        if (p == "veryslow") return "2";
        if (p == "placebo") return "0";
        return preset;
    }

    const bool hw = codec.find("nvenc") != std::string::npos ||
                    codec.find("vaapi") != std::string::npos ||
                    codec.find("qsv") != std::string::npos ||
                    codec.find("amf") != std::string::npos;
    if (!hw)
        return preset;

    if (p == "ultrafast" || p == "superfast") return "p1";
    if (p == "veryfast") return "p2";
    if (p == "faster") return "p2";
    if (p == "fast") return "p3";
    if (p == "medium") return "p4";
    if (p == "slow") return "p6";
    if (p == "veryslow" || p == "placebo") return "p7";
    return preset;
}

int qsv_preset_for(const std::string& codec, const std::string& preset) {
    (void)codec;
    const std::string p = [&] {
        std::string q = preset;
        for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return q;
    }();

    if (p == "ultrafast" || p == "superfast" || p == "veryfast") return 7;
    if (p == "faster") return 6;
    if (p == "fast") return 5;
    if (p == "medium") return 4;
    if (p == "slow") return 3;
    if (p == "slower") return 2;
    if (p == "veryslow" || p == "placebo") return 1;
    try {
        return std::stoi(preset);
    } catch (...) {
        return 4;
    }
}

bool export_project(const Project& project, const ExportSettings& s, ExportControl* control,
                    std::string* error) {
    const auto fail = [&](const std::string& m) {
        ::canvas::core::log::log_error("render failure: %s", m.c_str());
        if (error) *error = m;
        return false;
    };
    CANVAS_LOG("render: begin out='%s' fmt=%s codec=%s w=%dx%d fps=%.3f frames=%lld audio=%s",
           s.output_path.c_str(), s.format.c_str(), s.video_codec.c_str(), s.width, s.height,
           s.fps, (long long)s.duration_frames,
           (s.audio_codec.empty() ? "no" : s.audio_codec.c_str()));
    if (s.output_path.empty())
        return fail("Output path is missing. Choose an output folder on the Deliver panel.");
    if (s.video_codec.empty())
        return fail("No video codec selected.");

    const AVOutputFormat* out_fmt = av_guess_format(s.format.c_str(), s.output_path.c_str(), nullptr);
    if (!out_fmt) out_fmt = av_guess_format(nullptr, s.output_path.c_str(), nullptr);
    if (!out_fmt) return fail("Unknown output container: " + s.format);

    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, out_fmt, nullptr, s.output_path.c_str()) < 0 || !oc)
        return fail("Failed to allocate output context.");

    const auto cancelled = [&]() {
        return control && control->should_cancel && control->should_cancel();
    };
    const auto progress = [&](double p, const char* phase) {
        if (control && control->on_progress) control->on_progress(p, phase);
    };

    PreviewThrottle preview_throttle;
    const auto push_preview = [&](const VideoFramePtr& vf) {
        if (!control || !control->on_frame || !vf) return;
        control->on_frame(vf);
    };

    const AVCodec* vcodec = avcodec_find_encoder_by_name(s.video_codec.c_str());
    if (!vcodec) { avformat_free_context(oc); return fail("Unknown video encoder: " + s.video_codec); }

    AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) { avformat_free_context(oc); return fail("No video codec context."); }

    const std::string hw_device = hw_device_for_codec(s.video_codec);
    const bool hw_codec = !hw_device.empty();
    bool v_use_hw = false;
    const AVPixelFormat hw_pix = pick_video_fmt(vcodec, &v_use_hw);
    v_use_hw = v_use_hw && hw_codec;

    AVPixelFormat sw_pix = AV_PIX_FMT_YUV420P;
    if (hw_codec) {
        sw_pix = AV_PIX_FMT_NV12;
    } else if (hw_pix != AV_PIX_FMT_YUV420P && hw_pix != AV_PIX_FMT_NV12 &&
               !is_hw_pix_fmt(hw_pix)) {
        sw_pix = hw_pix;
    }

    vctx->width = s.width;
    vctx->height = s.height;
    const int ri = std::lround(s.fps);
    vctx->time_base = AVRational{1, std::max(1, ri)};
    vctx->framerate = AVRational{ri, 1};
    vctx->pix_fmt = v_use_hw ? hw_pix : sw_pix;
    vctx->gop_size = 120;
    vctx->max_b_frames = 0;
    if (s.threads > 0) vctx->thread_count = s.threads;
    vctx->color_range = AVCOL_RANGE_MPEG;
    vctx->colorspace = AVCOL_SPC_BT709;
    vctx->color_trc = AVCOL_TRC_BT709;
    vctx->color_primaries = AVCOL_PRI_BT709;
    vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (s.crf >= 0) {
        if (is_vaapi_codec(s.video_codec)) {
            apply_vaapi_rate_control(vctx, vaapi_rate_control_from(
                s.video_codec, s.crf, s.vid_rc_mode, s.video_bitrate_kbps));
        } else if (is_qsv_codec(s.video_codec)) {
            apply_qsv_rate_control(vctx, qsv_rate_control_from(
                s.video_codec, s.crf, s.vid_rc_mode, s.video_bitrate_kbps));
        } else {
            av_opt_set_int(vctx->priv_data, "crf", s.crf, 0);
        }
    }
    const bool is_nvenc = s.video_codec.find("nvenc") != std::string::npos;
    if (s.video_bitrate_kbps > 0 && s.crf < 0) {
        const int64_t bps = static_cast<int64_t>(s.video_bitrate_kbps) * 1000;
        vctx->bit_rate = bps;
        const bool cbr = s.vid_rc_mode == "cbr";
        const bool vbr = s.vid_rc_mode == "vbr_target";
        if (cbr || vbr) {
            const int64_t max_bps =
                s.video_max_bitrate_kbps > 0
                    ? static_cast<int64_t>(s.video_max_bitrate_kbps) * 1000
                    : bps;
            const int64_t bufsize_bps = cbr ? max_bps : max_bps * 2;
            av_opt_set_int(vctx, "maxrate", max_bps, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(vctx, "bufsize", bufsize_bps, AV_OPT_SEARCH_CHILDREN);
            if (is_nvenc)
                av_opt_set(vctx->priv_data, "rc", cbr ? "cbr" : "vbr", 0);
            else if (is_vaapi_codec(s.video_codec))
                av_opt_set(vctx->priv_data, "rc_mode", cbr ? "CBR" : "VBR", 0);
            else if (is_qsv_codec(s.video_codec))
                av_opt_set_int(vctx->priv_data, "rc_mode", cbr ? 42 : 0, 0);
        }
    }
    if (is_nvenc && s.crf >= 0 && s.vid_rc_mode == "constqp") {
        av_opt_set(vctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(vctx->priv_data, "cq", s.crf, 0);
    }
    if (!s.preset.empty())
    {
        const std::string p = s.preset;
        if (is_qsv_codec(s.video_codec)) {
            if (!p.empty())
                av_opt_set_int(vctx->priv_data, "preset",
                               qsv_preset_for(s.video_codec, p), 0);
        } else if (is_vaapi_codec(s.video_codec)) {
            const VaapiSpeed spd = vaapi_speed_for(s.video_codec, p);
            if (spd.async_depth > 0)
                av_opt_set_int(vctx->priv_data, "async_depth", spd.async_depth, 0);
            if (spd.quality > 0)
                av_opt_set_int(vctx->priv_data, "quality", spd.quality, 0);
        } else {
            av_opt_set(vctx->priv_data, "preset", nv_preset_for(s.video_codec, p).c_str(), 0);
        }
    }
    if (is_nvenc && s.video_bitrate_kbps > 0 && s.crf < 0) {
        av_opt_set_int(vctx->priv_data, "spatial_aq", 1, 0);
        av_opt_set_int(vctx->priv_data, "temporal_aq", 1, 0);
        av_opt_set_int(vctx->priv_data, "aq_strength", 8, 0);
    }
    apply_codec_extra(vctx, s.extra);

    const std::string vc = s.video_codec;
    if (vc.find("nvenc") != std::string::npos &&
        (vc.find("av1") != std::string::npos || vc.find("hevc") != std::string::npos ||
         vc.find("h265") != std::string::npos)) {
        av_opt_set_int(vctx->priv_data, "split_encode_mode", 1, 0);
    }
    if (is_nvenc && (vc.find("hevc") != std::string::npos ||
                     vc.find("h265") != std::string::npos)) {
        const double luma_sps =
            static_cast<double>(s.width) * static_cast<double>(s.height) * s.fps;
        int level = 183;
        if (luma_sps > 2.56e9) level = 186;
        if (s.video_bitrate_kbps > 0 && s.crf < 0) {
            const int64_t max_bps =
                s.video_max_bitrate_kbps > 0
                    ? static_cast<int64_t>(s.video_max_bitrate_kbps) * 1000
                    : static_cast<int64_t>(s.video_bitrate_kbps) * 1000;
            const int64_t cpb_bps = s.vid_rc_mode == "cbr" ? max_bps : max_bps * 2;
            if (cpb_bps > 120000000) level = 186;
        }
        av_opt_set_int(vctx->priv_data, "level", level, 0);
    }
    if (v_use_hw && !is_qsv_codec(s.video_codec)) {
        int rc_lookahead = 0;
        const std::string& ex = s.extra;
        const std::string key = "rc-lookahead=";
        std::size_t pos = ex.find(key);
        if (pos != std::string::npos) {
            std::size_t end = ex.find_first_of("\r\n", pos);
            const std::string val = ex.substr(pos + key.size(), end == std::string::npos
                                                                ? std::string::npos
                                                                : end - (pos + key.size()));
            rc_lookahead = std::atoi(val.c_str());
        }
        const int surfaces = rc_lookahead > 0 ? rc_lookahead + 10 : 12;
        av_opt_set_int(vctx->priv_data, "surfaces", surfaces, 0);
    }

    FramePump pump;
    if (v_use_hw &&
        pump.create_device_and_pool(hw_device, hw_pix, s.width, s.height)) {
        vctx->hw_frames_ctx = av_buffer_ref(pump.hw_frames);
    } else {
        v_use_hw = false;
    }

    if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
        pump.teardown();
        avcodec_free_context(&vctx);
        avformat_free_context(oc);
        return fail("Failed to open video encoder: " + s.video_codec);
    }
    fprintf(stderr, "[dbg] after open: vctx time_base=%d/%d framerate=%d/%d pix_fmt=%d\n",
            vctx->time_base.num, vctx->time_base.den, vctx->framerate.num, vctx->framerate.den, vctx->pix_fmt);

    AVStream* vst = avformat_new_stream(oc, nullptr);
    if (!vst) { avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No video stream."); }
    vst->id = static_cast<int>(oc->nb_streams);
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vst->time_base = vctx->time_base;
    vst->r_frame_rate = vctx->framerate;
    vst->avg_frame_rate = vctx->framerate;
    fprintf(stderr, "[dbg] vst->time_base set to %d/%d\n", vst->time_base.num, vst->time_base.den);

    const bool do_audio = !s.remove_audio && !s.audio_codec.empty() && s.duration_frames > 0;
    AVCodecContext* actx = nullptr;
    AVStream* ast = nullptr;
    int a_frame_size = 0;
    int a_src_samples_ = 0;
    if (do_audio) {
        const AVCodec* acodec = avcodec_find_encoder_by_name(s.audio_codec.c_str());
        if (!acodec) {
            avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Unknown audio encoder: " + s.audio_codec);
        }
        actx = avcodec_alloc_context3(acodec);
        if (!actx) { avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No audio ctx"); }
        actx->sample_rate = s.audio_sample_rate;
        av_channel_layout_default(&actx->ch_layout, s.audio_channels);
        actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        actx->bit_rate = static_cast<int64_t>(s.audio_bitrate_kbps) * 1000;
        if (avcodec_open2(actx, acodec, nullptr) < 0) {
            avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Failed to open audio encoder: " + s.audio_codec);
        }
        a_frame_size = std::max(1, actx->frame_size > 0 ? actx->frame_size : 1024);
        a_src_samples_ = std::max(a_frame_size,
            (int)std::max<int64_t>(1, (int64_t)std::llround((double)s.audio_sample_rate / s.fps)));
        ast = avformat_new_stream(oc, nullptr);
        if (!ast) { avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc); return fail("No audio stream."); }
        ast->id = static_cast<int>(oc->nb_streams);
        avcodec_parameters_from_context(ast->codecpar, actx);
        ast->time_base = AVRational{1, s.audio_sample_rate};
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, ("file:" + s.output_path).c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&actx); avcodec_free_context(&vctx); avformat_free_context(oc);
            return fail("Cannot open output file: " + s.output_path);
        }
    }

    if (!s.chapters.empty()) {
        for (const auto& ch : s.chapters) {
            AVChapter* c = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            if (!c) continue;
            c->time_base = AVRational{1, 1000};
            c->start = static_cast<int64_t>(std::llround(ch.start_seconds * 1000.0));
            c->end = static_cast<int64_t>(std::llround(ch.end_seconds * 1000.0));
            if (c->end <= c->start) c->end = c->start + 1;
            c->id = static_cast<int>(oc->nb_chapters);
            av_dict_set(&c->metadata, "title", ch.title.c_str(), 0);
            int nch = static_cast<int>(oc->nb_chapters);
            av_dynarray_add(&oc->chapters, &nch, c);
            oc->nb_chapters = static_cast<unsigned>(nch);
        }
    }

    const int wh_ret = avformat_write_header(oc, nullptr);
    if (wh_ret < 0) {
        char ebuf[128] = {0};
        av_strerror(wh_ret, ebuf, sizeof(ebuf));
        fprintf(stderr, "[wrh] ret=%d (%s) nstreams=%d\n", wh_ret, ebuf, (int)oc->nb_streams);
        for (unsigned i = 0; i < oc->nb_streams; ++i) {
            AVStream* st = oc->streams[i];
            AVCodecParameters* cp = st->codecpar;
            fprintf(stderr, "  st%u type=%d codec=%s fmt=%d tb=%d/%d w=%d h=%d sr=%d ch=%d\n",
                    i, cp->codec_type, cp->codec_id == AV_CODEC_ID_NONE ? "?" : "?",
                    cp->format, st->time_base.num, st->time_base.den,
                    cp->width, cp->height, cp->sample_rate,
                    cp->ch_layout.nb_channels);
        }
        avcodec_free_context(&actx); avcodec_free_context(&vctx);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return fail("Failed to write container header.");
    }

    progress(0.0, "Encode");

    const AVPixelFormat enc_sw_fmt = v_use_hw ? AV_PIX_FMT_NV12 : vctx->pix_fmt;
    const bool tagged_bt709 = (vctx->colorspace == AVCOL_SPC_BT709);
    if (!pump.init_converters(s.width, s.height, enc_sw_fmt, tagged_bt709)) {
        avcodec_free_context(&actx); avcodec_free_context(&vctx);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return fail("Failed to create RGB->YUV conversion context.");
    }
    ::canvas::core::log::log_warning(
        "[export] color audit: out_tags=range:%s/matrix:%s/trc:%s ; rgba->%s "
        "sws_setColorspaceDetails matrix=%s srcRange=JPEG dstRange=MPEG — %s",
        vctx->color_range == AVCOL_RANGE_MPEG ? "mpeg" : "jpeg",
        vctx->colorspace == AVCOL_SPC_BT709 ? "bt709"
            : vctx->colorspace == AVCOL_SPC_BT470BG ? "bt601" : "other",
        vctx->color_trc == AVCOL_TRC_BT709 ? "bt709" : "other",
        av_get_pix_fmt_name(enc_sw_fmt),
        pump.matrix_name(),
        tagged_bt709 ? "conversion matches stamped matrix"
                     : "conversion matrix follows tagged colorspace");

    AVFrame* a_src = nullptr;
    if (do_audio) {
        a_src = av_frame_alloc();
        a_src->format = AV_SAMPLE_FMT_FLTP;
        a_src->sample_rate = s.audio_sample_rate;
        av_channel_layout_copy(&a_src->ch_layout, &actx->ch_layout);
        a_src->nb_samples = a_src_samples_ > 0 ? a_src_samples_ : a_frame_size;
        av_frame_get_buffer(a_src, 0);
    }

    const double seq_fps = project.sequence.fps;
    const double export_fps = s.fps > 0.0 ? s.fps : (seq_fps > 0.0 ? seq_fps : 30.0);
    const double tl_per_frame = seq_fps > 0.0 ? seq_fps / export_fps : 1.0;
    pump.tl_per_frame = tl_per_frame;
    const int64_t total_video = seq_fps > 0.0
        ? (int64_t)std::llround((double)s.duration_frames / seq_fps * export_fps)
        : s.duration_frames;
    const int64_t total_audio = total_video > 0
        ? (int64_t)std::llround((double)total_video / export_fps * s.audio_sample_rate)
        : 0;

    int64_t frame = 0;
    int64_t audio_sample = 0;
    bool ended = false;

    canvas::core::log::RenderTelemetry telemetry;

    std::vector<float> a_acc;
    int64_t a_sent = 0;
    if (do_audio && a_frame_size > 0)
        a_acc.reserve((std::size_t)a_frame_size * 2 * s.audio_channels);

    RenderSession session;
    session.set_telemetry(&telemetry);
    bool session_ok = session.begin(project, s.width, s.height, pump.dec_dev);

    pump.session = &session;
    pump.project = &project;
    pump.session_ok = session_ok;
    pump.telemetry = &telemetry;
    pump.throttle = &preview_throttle;
    pump.push_preview = push_preview;

    const std::size_t producer_depth = 64;
    std::mutex qmu;
    std::condition_variable qcv;
    struct ProducerSlot {
        AVFrame* frame = nullptr;
        void* event = nullptr;
        AVFrame* source = nullptr;
    };
    std::deque<ProducerSlot> ready_frames;
    bool producer_done = false;
    auto render_one_frame = [&](const int64_t f) -> std::tuple<AVFrame*, void*, AVFrame*> {
        const int64_t tl = (int64_t)std::llround((double)f * tl_per_frame);
        PumpFrame slot = pump.produce(tl, f);
        return {slot.frame, slot.event, slot.source};
    };

    auto producer_thread_fn = [&] {
        int64_t f = 0;
        while (f < total_video) {
            auto [frm, ev, src] = render_one_frame(f);
            if (ev) {
                canvas::core::gpu::convert_nv12_wait_event(ev);
                if (src) av_frame_unref(src);
            }
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait(lk, [&] {
                return cancelled() || ready_frames.size() < producer_depth;
            });
            if (cancelled()) {
                lk.unlock();
                av_frame_free(&frm);
                if (ev) canvas::core::gpu::convert_nv12_destroy_event(ev);
                break;
            }
            ready_frames.push_back({frm, ev, nullptr});
            lk.unlock();
            qcv.notify_all();
            ++f;
        }
        {
            std::lock_guard<std::mutex> lk(qmu);
            producer_done = true;
        }
        qcv.notify_all();
    };

    if (session_ok && total_video > 0 && pump.kind != ExportBackend::Software) {
        std::thread producer(producer_thread_fn);

        while (!ended && !cancelled()) {
            AVFrame* to_send = nullptr;
            void* ev = nullptr;
            AVFrame* src = nullptr;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] {
                    return !ready_frames.empty() || producer_done || cancelled();
                });
                if (!ready_frames.empty()) {
                    auto slot = ready_frames.front();
                    ready_frames.pop_front();
                    to_send = slot.frame;
                    ev = slot.event;
                    src = slot.source;
                    telemetry.observe_queue(ready_frames.size());
                }
            }
            qcv.notify_all();

            if (ev) canvas::core::gpu::convert_nv12_wait_event(ev);
            if (src) av_frame_unref(src);

            const auto enc_t0 = std::chrono::steady_clock::now();
            if (to_send) {
                avcodec_send_frame(vctx, to_send);
                pump.after_send();
                av_frame_free(&to_send);
            }
            ++frame;

            AVPacket* pkt = av_packet_alloc();
            while (avcodec_receive_packet(vctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, vctx->time_base, vst->time_base);
                pkt->stream_index = vst->index;
                av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
            }
            if (do_audio && actx) {
                while (avcodec_receive_packet(actx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, actx->time_base, ast->time_base);
                    pkt->stream_index = ast->index;
                    av_interleaved_write_frame(oc, pkt);
                    av_packet_unref(pkt);
                }
            }
            av_packet_free(&pkt);
            telemetry.note_encode(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - enc_t0).count());
            telemetry.set_progress(frame, total_video);
            pump.account_stalls();
            telemetry.tick();

            if (do_audio && audio_sample < total_audio) {
                const int per_frame = (int)std::max<int64_t>(1,
                    (int64_t)std::llround((double)s.audio_sample_rate / s.fps));
                const auto audio_t0 = std::chrono::steady_clock::now();
                auto ac = session.audio_chunk(audio_sample, per_frame,
                                              s.audio_sample_rate, s.audio_channels, s.fps);
                telemetry.note_audio(std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - audio_t0).count());
                const int n = (ac && !ac->samples.empty())
                    ? (int)(ac->samples.size() / s.audio_channels)
                    : 0;
                if (n > 0)
                    a_acc.insert(a_acc.end(), ac->samples.begin(), ac->samples.end());
                audio_sample += std::max<int64_t>(n, per_frame);
                const std::size_t ch = (std::size_t)s.audio_channels;
                while (a_acc.size() >= (std::size_t)a_frame_size * ch) {
                    for (std::size_t c = 0; c < ch; ++c)
                        for (int k = 0; k < a_frame_size; ++k)
                            ((float*)a_src->extended_data[c])[k] =
                                a_acc[(std::size_t)k * ch + c];
                    a_src->nb_samples = a_frame_size;
                    a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                    avcodec_send_frame(actx, a_src);
                    ++a_sent;
                    a_acc.erase(a_acc.begin(), a_acc.begin() + (std::size_t)a_frame_size * ch);
                }
            }

            if (frame >= total_video && (audio_sample >= total_audio || !do_audio)) {
                if (do_audio && actx && !a_acc.empty()) {
                    const std::size_t ch = (std::size_t)s.audio_channels;
                    const int tail = (int)(a_acc.size() / ch);
                    for (std::size_t c = 0; c < ch; ++c)
                        for (int k = 0; k < tail; ++k)
                            ((float*)a_src->extended_data[c])[k] =
                                a_acc[(std::size_t)k * ch + c];
                    a_src->nb_samples = tail;
                    a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                    avcodec_send_frame(actx, a_src);
                    ++a_sent;
                    a_acc.clear();
                }
                avcodec_send_frame(vctx, nullptr);
                while (true) {
                    AVPacket* p2 = av_packet_alloc();
                    int r = avcodec_receive_packet(vctx, p2);
                    if (r < 0) { av_packet_free(&p2); break; }
                    av_packet_rescale_ts(p2, vctx->time_base, vst->time_base);
                    p2->stream_index = vst->index;
                    av_interleaved_write_frame(oc, p2);
                    av_packet_free(&p2);
                }
                if (do_audio && actx) {
                    avcodec_send_frame(actx, nullptr);
                    while (true) {
                        AVPacket* p2 = av_packet_alloc();
                        int r = avcodec_receive_packet(actx, p2);
                        if (r < 0) { av_packet_free(&p2); break; }
                        av_packet_rescale_ts(p2, actx->time_base, ast->time_base);
                        p2->stream_index = ast->index;
                        av_interleaved_write_frame(oc, p2);
                        av_packet_free(&p2);
                    }
                }
                ended = true;
            }

            if (total_video > 0)
                progress((double)std::min(frame, total_video) / total_video, "Encode");
        }
        if (producer.joinable()) producer.join();
        {
            std::lock_guard<std::mutex> lk(qmu);
            while (!ready_frames.empty()) {
                auto slot = ready_frames.front();
                ready_frames.pop_front();
                av_frame_free(&slot.frame);
                if (slot.event) canvas::core::gpu::convert_nv12_destroy_event(slot.event);
                if (slot.source) av_frame_unref(slot.source);
            }
            producer_done = true;
        }
    } else {

    while (!ended && !cancelled()) {
        const int64_t tl = (int64_t)std::llround((double)frame * tl_per_frame);
        if (frame < total_video) {
            PumpFrame slot = pump.produce(tl, frame);
            if (slot.frame) {
                avcodec_send_frame(vctx, slot.frame);
                pump.after_send();
                av_frame_free(&slot.frame);
            }
            if (slot.event) canvas::core::gpu::convert_nv12_destroy_event(slot.event);
            if (slot.source) av_frame_unref(slot.source);
        }
        ++frame;

        if (do_audio && audio_sample < total_audio) {
            const int per_frame = (int)std::max<int64_t>(1,
                (int64_t)std::llround((double)s.audio_sample_rate / s.fps));
            const auto audio_t0 = std::chrono::steady_clock::now();
            auto ac = session.audio_chunk(audio_sample, per_frame,
                                          s.audio_sample_rate, s.audio_channels, s.fps);
            telemetry.note_audio(std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - audio_t0).count());
            const int n = (ac && !ac->samples.empty())
                ? (int)(ac->samples.size() / s.audio_channels)
                : 0;
            if (n > 0)
                a_acc.insert(a_acc.end(), ac->samples.begin(), ac->samples.end());
            audio_sample += std::max<int64_t>(n, per_frame);
            const std::size_t ch = (std::size_t)s.audio_channels;
            while (a_acc.size() >= (std::size_t)a_frame_size * ch) {
                for (std::size_t c = 0; c < ch; ++c)
                    for (int k = 0; k < a_frame_size; ++k)
                        ((float*)a_src->extended_data[c])[k] =
                            a_acc[(std::size_t)k * ch + c];
                a_src->nb_samples = a_frame_size;
                a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                avcodec_send_frame(actx, a_src);
                ++a_sent;
                a_acc.erase(a_acc.begin(), a_acc.begin() + (std::size_t)a_frame_size * ch);
            }
        }

        const auto cpu_enc_t0 = std::chrono::steady_clock::now();
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(vctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, vctx->time_base, vst->time_base);
            pkt->stream_index = vst->index;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        if (do_audio && actx) {
            while (avcodec_receive_packet(actx, pkt) == 0) {
                av_packet_rescale_ts(pkt, actx->time_base, ast->time_base);
                pkt->stream_index = ast->index;
                av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
        telemetry.note_encode(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpu_enc_t0).count());
        telemetry.set_progress(frame, total_video);
        pump.account_stalls();
        telemetry.tick();

        if (frame >= total_video && (audio_sample >= total_audio || !do_audio)) {
            if (do_audio && actx && !a_acc.empty()) {
                const std::size_t ch = (std::size_t)s.audio_channels;
                const int tail = (int)(a_acc.size() / ch);
                for (std::size_t c = 0; c < ch; ++c)
                    for (int k = 0; k < tail; ++k)
                        ((float*)a_src->extended_data[c])[k] =
                            a_acc[(std::size_t)k * ch + c];
                a_src->nb_samples = tail;
                a_src->pts = (int64_t)a_frame_size * (int64_t)(a_sent);
                avcodec_send_frame(actx, a_src);
                ++a_sent;
                a_acc.clear();
            }
            avcodec_send_frame(vctx, nullptr);
            while (true) {
                AVPacket* p2 = av_packet_alloc();
                int r = avcodec_receive_packet(vctx, p2);
                if (r < 0) { av_packet_free(&p2); break; }
                av_packet_rescale_ts(p2, vctx->time_base, vst->time_base);
                p2->stream_index = vst->index;
                av_interleaved_write_frame(oc, p2);
                av_packet_free(&p2);
            }
            if (do_audio && actx) {
                avcodec_send_frame(actx, nullptr);
                while (true) {
                    AVPacket* p2 = av_packet_alloc();
                    int r = avcodec_receive_packet(actx, p2);
                    if (r < 0) { av_packet_free(&p2); break; }
                    av_packet_rescale_ts(p2, actx->time_base, ast->time_base);
                    p2->stream_index = ast->index;
                    av_interleaved_write_frame(oc, p2);
                    av_packet_free(&p2);
                }
            }
            ended = true;
        }

        if (total_video > 0)
            progress((double)std::min(frame, total_video) / total_video, "Encode");
    }

    }

    av_write_trailer(oc);

    telemetry.flush();

    CANVAS_LOG("render: complete out='%s' frames=%lld audio_samples=%lld",
           s.output_path.c_str(), (long long)frame, (long long)audio_sample);

    pump.teardown();

    if (a_src) av_frame_free(&a_src);
    avcodec_free_context(&actx);
    avcodec_free_context(&vctx);
    if (oc && oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);

    if (cancelled()) return fail("Export cancelled.");
    return true;
}

}
