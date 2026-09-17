#include "canvas/core/media/sw_decode.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <cstdlib>

namespace canvas::core {

void read_stall_tick(const double ms) {
    static auto s_at = std::chrono::steady_clock::now();
    static int s_total = 0, s_stalls = 0;
    static double s_over_ms = 0.0, s_max = 0.0;
    ++s_total;
    if (ms >= 20.0) {
        ++s_stalls;
        s_over_ms += ms - 20.0;
        s_max = std::max(s_max, ms);
    }
    const auto now = std::chrono::steady_clock::now();
    if (s_total == 1 || now - s_at >= std::chrono::seconds(1)) {
        s_at = now;
        if (s_stalls > 0)
            ::canvas::core::log::log_warning(
                "[io] reads=%d stalls_ge20ms=%d over_ms=%.0f max_ms=%.0f",
                s_total, s_stalls, s_over_ms, s_max);
        s_total = 0;
        s_stalls = 0;
        s_over_ms = 0.0;
        s_max = 0.0;
    }
}

static int s_decode_fail_burst = 0;

void decode_fail(const char* where) {
    if (++s_decode_fail_burst == 3)
        ::canvas::core::log::log_warning("[dec] fail_burst=%d where=%s",
                                         s_decode_fail_burst, where);
}

void decode_ok() {
    s_decode_fail_burst = 0;
}

void DemuxState::close() {
    if (packet) av_packet_free(&packet);
    if (av_frame) av_frame_free(&av_frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    hw_pix_fmt = AV_PIX_FMT_NONE;
    hw_avail = false;
    video_stream = -1;
    stream_tb = {0, 1};
    width = 0;
    height = 0;
    frame_rate = 0.0;
    duration_seconds = 0.0;
    total_frames = -1;
    last_frame = -1;
    next_frame = 0;
    draining = false;
    path.clear();
}

void DemuxState::reset_stream(const int64_t resume_frame) {
    if (codec_ctx) avcodec_flush_buffers(codec_ctx);
    draining = false;
    next_frame = resume_frame;
}

int64_t DemuxState::clamp_target(const int64_t target) const {
    int64_t t = target < 0 ? 0 : target;
    if (last_frame > 0) t = std::min(t, last_frame);
    return t;
}

void DemuxState::refine_last_frame() {
    if (last_frame > 0 || frame_rate <= 0.0) return;
    if (fmt_ctx && video_stream >= 0) {
        const AVStream* st = fmt_ctx->streams[video_stream];
        double secs = st->duration > 0 ? static_cast<double>(st->duration) * av_q2d(stream_tb) : 0.0;
        if (secs <= 0.0 && fmt_ctx->duration > 0 && fmt_ctx->duration != AV_NOPTS_VALUE)
            secs = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
        if (secs <= 0.0 && duration_seconds > 0.0) secs = duration_seconds;
        if (secs > 0.0) last_frame = std::max<int64_t>(0, static_cast<int64_t>(
            std::llround(secs * frame_rate)));
    }
}

void DemuxState::container_seek_seconds(const double target_seconds) {
    const auto us = static_cast<int64_t>(
        std::llround(target_seconds * static_cast<double>(AV_TIME_BASE)));
    CANVAS_LOG("video_decoder: container_seek_seconds %.3fs us=%lld", target_seconds, (long long)us);
    if (avformat_seek_file(fmt_ctx, -1, INT64_MIN, us, us, 0) < 0) {
        CANVAS_LOG("video_decoder: container_seek retry to 0");
        avformat_seek_file(fmt_ctx, -1, INT64_MIN, 0, 0, 0);
    }
    reset_stream(0);
}

bool DemuxState::read_video_packet() {
    for (;;) {
        const auto rd_t0 = std::chrono::steady_clock::now();
        const int r = av_read_frame(fmt_ctx, packet);
        read_stall_tick(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - rd_t0).count());
        if (r < 0) {
            draining = true;
            avcodec_send_packet(codec_ctx, nullptr);
            return false;
        }
        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }
        avcodec_send_packet(codec_ctx, packet);
        av_packet_unref(packet);
        return true;
    }
}

namespace {

std::shared_ptr<const std::vector<IframeEntry>> build_iframe_sync(const std::string& path,
                                                                  const AVRational stream_tb,
                                                                  const double fps,
                                                                  const int vstream) {
    auto out = std::make_shared<std::vector<IframeEntry>>();
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) return nullptr;
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return out;
    }
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&ctx);
        return out;
    }
    for (;;) {
        const int r = av_read_frame(ctx, pkt);
        if (r < 0) break;
        if (pkt->stream_index == vstream && (pkt->flags & AV_PKT_FLAG_KEY)) {
            double secs = 0.0;
            const int64_t ticks = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (ticks != AV_NOPTS_VALUE) secs = static_cast<double>(ticks) * av_q2d(stream_tb);
            const int64_t frame = fps > 0.0
                ? std::max<int64_t>(static_cast<int64_t>(std::llround(secs * fps)), 0)
                : -1;
            if (frame >= 0) out->push_back({pkt->pos, secs, frame});
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return out;
}

}

bool DemuxState::has_iframe_index() const {
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lk(s_iframe_mtx);
    const auto it = s_iframe_cache.find(path);
    return it != s_iframe_cache.end() && it->second && it->second->size() > 1;
}

void DemuxState::build_iframe_index() {
    if (path.empty() || video_stream < 0 || frame_rate <= 0.0) return;
    const std::string p = path;
    const AVRational stb = stream_tb;
    const double fps = frame_rate;
    const int vstream = video_stream;
    {
        std::lock_guard<std::mutex> lk(s_iframe_mtx);
        if (s_iframe_cache.count(p) || !s_iframe_inflight.insert(p).second) return;
    }
    std::thread([p, stb, fps, vstream] {
        const auto ib_t0 = std::chrono::steady_clock::now();
        auto built = build_iframe_sync(p, stb, fps, vstream);
        const double ib_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - ib_t0).count();
        std::lock_guard<std::mutex> lk2(s_iframe_mtx);
        if (built && built->size() > 1 && !s_iframe_cache.count(p))
            s_iframe_cache[p] = std::move(built);
        s_iframe_inflight.erase(p);
        const double gop_s = (built && built->size() > 2)
            ? (built->back().frame - built->front().frame) /
                  (static_cast<double>(built->size()) * std::max(fps, 1.0))
            : 0.0;
        ::canvas::core::log::log_warning(
            "[dec] iframe_index path=%s entries=%zu ms=%.0f gop_secs=%.2f cached=%d",
            p.c_str(), built ? built->size() : 0, ib_ms, gop_s,
            s_iframe_cache.count(p) > 0);
        CANVAS_LOG("video_decoder: iframe_index built for '%s' entries=%zu cached=%d",
               p.c_str(), built ? built->size() : 0,
               s_iframe_cache.count(p) > 0);
    }).detach();
}

const IframeEntry* DemuxState::iframe_at_or_before(const int64_t target) const {
    if (target < 0 || path.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(s_iframe_mtx);
    auto it = s_iframe_cache.find(path);
    if (it == s_iframe_cache.end()) return nullptr;
    const IframeEntry* best = nullptr;
    for (const IframeEntry& e : *it->second) {
        if (e.frame <= target) best = &e;
        else break;
    }
    return best;
}

void SoftDecoder::close() {
    demux_ = nullptr;
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    hold_rgba_.reset();
    hold_rgba_src_ = -1;
    hold_rgba_dim_ = -1;
    out_max_dim_ = 0;
    path_seq_ = path_seeks_ = 0;
    path_seq_ms_ = path_seek_ms_ = 0.0;
    convert_ms_ = 0.0;
}

SoftDecoder::SoftDecoder(SoftDecoder&& o) noexcept { *this = std::move(o); }

SoftDecoder& SoftDecoder::operator=(SoftDecoder&& o) noexcept {
    if (this == &o) return *this;
    close();
    demux_ = o.demux_;
    o.demux_ = nullptr;
    sws_ctx_ = o.sws_ctx_;
    o.sws_ctx_ = nullptr;
    out_max_dim_ = o.out_max_dim_;
    o.out_max_dim_ = 0;
    hold_rgba_ = std::move(o.hold_rgba_);
    hold_rgba_src_ = o.hold_rgba_src_;
    o.hold_rgba_src_ = -1;
    hold_rgba_dim_ = o.hold_rgba_dim_;
    o.hold_rgba_dim_ = -1;
    path_seq_ = o.path_seq_;
    o.path_seq_ = 0;
    path_seeks_ = o.path_seeks_;
    o.path_seeks_ = 0;
    path_seq_ms_ = o.path_seq_ms_;
    o.path_seq_ms_ = 0.0;
    path_seek_ms_ = o.path_seek_ms_;
    o.path_seek_ms_ = 0.0;
    convert_ms_ = o.convert_ms_;
    o.convert_ms_ = 0.0;
    return *this;
}

void SoftDecoder::reset_path_counters() {
    path_seq_ = path_seeks_ = 0;
    path_seq_ms_ = path_seek_ms_ = 0.0;
    convert_ms_ = 0.0;
}

VideoFramePtr SoftDecoder::decode_next() {
    if (!demux_ || !demux_->codec_ctx || demux_->frame_rate <= 0.0) {
        CANVAS_LOG("video_decoder: decode_next SKIP (codec=%p fps=%.3f)",
               (void*)(demux_ ? demux_->codec_ctx : nullptr),
               demux_ ? demux_->frame_rate : 0.0);
        return nullptr;
    }
    DemuxState& d = *demux_;
    for (;;) {
        const int ret = avcodec_receive_frame(d.codec_ctx, d.av_frame);
        if (ret == 0) {
            int64_t ticks = d.av_frame->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = d.av_frame->pts;
            const bool have_ts = ticks != AV_NOPTS_VALUE;
            const double secs = have_ts
                ? static_cast<double>(ticks) * av_q2d(d.stream_tb)
                : static_cast<double>(d.next_frame) / d.frame_rate;
            const int64_t number =
                std::max<int64_t>(static_cast<int64_t>(std::llround(secs * d.frame_rate)), 0);
            auto out = convert_to_rgba(d.av_frame, ticks, secs, number);
            av_frame_unref(d.av_frame);
            if (!out) continue;
            d.next_frame = number + 1;
            decode_ok();
            return out;
        }
        if (ret == AVERROR_EOF) {
            if (d.next_frame > 0 && (d.last_frame < 0 || d.next_frame - 1 < d.last_frame))
                d.last_frame = d.next_frame - 1;
            CANVAS_LOG("video_decoder: decode_next EOF at frame %lld", (long long)d.next_frame);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_next ERROR ret=%d at frame %lld",
                            ret, (long long)d.next_frame);
            decode_fail("decode_next");
            return nullptr;
        }
        if (d.draining) return nullptr;
        if (!d.read_video_packet()) continue;
    }
}

VideoFramePtr SoftDecoder::decode_forward_to(const int64_t target, const int max_over) {
    if (!demux_ || !demux_->codec_ctx || demux_->frame_rate <= 0.0) return nullptr;
    DemuxState& d = *demux_;
    d.refine_last_frame();
    const auto df_t0 = std::chrono::steady_clock::now();
    int fast_over = 0;
    int64_t last_number = -1;
    int64_t last_ticks = AV_NOPTS_VALUE;
    double last_secs = 0.0;
    for (;;) {
        const int ret = avcodec_receive_frame(d.codec_ctx, d.av_frame);
        if (ret == 0) {
            int64_t ticks = d.av_frame->best_effort_timestamp;
            if (ticks == AV_NOPTS_VALUE) ticks = d.av_frame->pts;
            const bool have_ts = ticks != AV_NOPTS_VALUE;
            const double secs = have_ts
                ? static_cast<double>(ticks) * av_q2d(d.stream_tb)
                : static_cast<double>(d.next_frame) / d.frame_rate;
            const int64_t number =
                std::max<int64_t>(static_cast<int64_t>(std::llround(secs * d.frame_rate)), 0);
            last_number = number; last_ticks = ticks; last_secs = secs;

            if (number < target) {
                ++fast_over;
                av_frame_unref(d.av_frame);
                d.next_frame = number + 1;
                if (max_over > 0 && fast_over >= max_over) {
                    if (!d.draining) {
                        for (;;) {
                            const int r2 = avcodec_receive_frame(d.codec_ctx, d.av_frame);
                            if (r2 == 0) break;
                            if (r2 == AVERROR_EOF) { d.draining = true; return nullptr; }
                            if (r2 != AVERROR(EAGAIN)) return nullptr;
                            if (!d.read_video_packet()) return nullptr;
                        }
                    } else {
                        return nullptr;
                    }
                    auto out = convert_to_rgba(d.av_frame, last_ticks, last_secs, last_number);
                    av_frame_unref(d.av_frame);
                    if (!out) return nullptr;
                    d.next_frame = last_number + 1;
                    const double df_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - df_t0).count();
CANVAS_LOG(
                    "vdecode forward target=%lld got=%lld CAPPED fast_over_frames=%d max=%d ms=%.2f",
                    (long long)target, (long long)last_number, fast_over, max_over, df_ms);
                    decode_ok();
                    return out;
                }
                continue;
            }
            auto out = convert_to_rgba(d.av_frame, ticks, secs, number);
            av_frame_unref(d.av_frame);
            if (!out) continue;
            d.next_frame = number + 1;
            const double df_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - df_t0).count();
            CANVAS_LOG(
                "vdecode forward target=%lld got=%lld fast_over_frames=%d ms=%.2f",
                (long long)target, (long long)number, fast_over, df_ms);
            decode_ok();
            return out;
        }
        if (ret == AVERROR_EOF) {
            if (d.next_frame > 0 && (d.last_frame < 0 || d.next_frame - 1 < d.last_frame))
                d.last_frame = d.next_frame - 1;
            CANVAS_LOG("video_decoder: decode_forward_to EOF target=%lld at frame %lld",
                   (long long)target, (long long)d.next_frame);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            log::log_error("video_decoder: decode_forward_to ERROR ret=%d target=%lld",
                            ret, (long long)target);
            decode_fail("decode_forward_to");
            return nullptr;
        }
        if (d.draining) return nullptr;
        if (!d.read_video_packet()) continue;
    }
}

VideoFramePtr SoftDecoder::convert_to_rgba(const AVFrame* src, const int64_t ticks,
                                           const double seconds, const int64_t number) {
    if (!src || !demux_) return nullptr;
    DemuxState& d = *demux_;
    auto out = std::make_shared<VideoFrame>();

    const AVFrame* cvt = src;
    AVFrame* sw = nullptr;
    if (src->hw_frames_ctx) {
        sw = av_frame_alloc();
        if (!sw || av_hwframe_transfer_data(sw, src, 0) < 0) {
            CANVAS_LOG("decode: hw->cpu transfer FAILED frame=%lld", (long long)number);
            av_frame_free(&sw);
            return nullptr;
        }
        av_frame_copy_props(sw, src);
        cvt = sw;
    }

    int out_w = cvt->width;
    int out_h = cvt->height;
    if (out_max_dim_ > 0 && out_max_dim_ < std::max(cvt->width, cvt->height)) {
        if (cvt->width >= cvt->height) {
            out_w = out_max_dim_;
            out_h = std::max(1, static_cast<int>(std::llround(
                                    static_cast<double>(cvt->height) * out_max_dim_ / cvt->width)));
        } else {
            out_h = out_max_dim_;
            out_w = std::max(1, static_cast<int>(std::llround(
                                    static_cast<double>(cvt->width) * out_max_dim_ / cvt->height)));
        }
    }

    out->width = out_w;
    out->height = out_h;
    out->pts_ticks = ticks;
    out->pts_seconds = seconds;
    out->frame_number = number;

    const int align = 32;
    const int buf_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, out_w, out_h, align);
    if (buf_bytes <= 0) {
        log::log_error("video_decoder: rgbsize FAILED dst=%dx%d", out_w, out_h);
        av_frame_free(&sw);
        return nullptr;
    }
    const std::size_t sws_tail_pad = 64;
    out->rgba.resize(static_cast<std::size_t>(buf_bytes) + sws_tail_pad);
    uint8_t* dst_data[AV_NUM_DATA_POINTERS] = {nullptr};
    int dst_linesize[AV_NUM_DATA_POINTERS] = {0};
    if (av_image_fill_arrays(dst_data, dst_linesize, out->rgba.data(), AV_PIX_FMT_RGBA,
                             out_w, out_h, align) < 0) {
        log::log_error("video_decoder: rgbfill FAILED dst=%dx%d", out_w, out_h);
        av_frame_free(&sw);
        return nullptr;
    }
    out->stride = static_cast<std::size_t>(dst_linesize[0]);

    const bool scaled = (out_w != cvt->width) || (out_h != cvt->height);
    const auto conv_t0 = std::chrono::steady_clock::now();
    sws_ctx_ = sws_getCachedContext(sws_ctx_, cvt->width, cvt->height,
                                    static_cast<AVPixelFormat>(cvt->format),
                                    out_w, out_h, AV_PIX_FMT_RGBA,
                                    scaled ? SWS_FAST_BILINEAR : SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        log::log_error("video_decoder: make_rgba sws_getCachedContext FAILED src=%dx%d fmt=%d dst=%dx%d",
                        cvt->width, cvt->height, cvt->format, out_w, out_h);
        av_frame_free(&sw);
        return nullptr;
    }
    const int src_range = (d.range == gpu::ColorRange::Full) ? 1 : 0;
    int sws_matrix = SWS_CS_ITU709;
    switch (d.matrix) {
        case gpu::ColorMatrix::BT601: sws_matrix = SWS_CS_ITU601; break;
        case gpu::ColorMatrix::BT2020: sws_matrix = SWS_CS_BT2020; break;
        default: break;
    }
    const int* cs_coefs = sws_getCoefficients(sws_matrix);
    sws_setColorspaceDetails(sws_ctx_, cs_coefs, src_range, cs_coefs, 1,
                             0, 1 << 16, 1 << 16);
    static std::mutex sws_log_mu;
    static std::set<std::tuple<int, int, int>> sws_logged;
    const bool first_cvt = [&] {
        std::lock_guard<std::mutex> lk(sws_log_mu);
        return sws_logged.insert({static_cast<int>(cvt->format), cvt->width, cvt->height}).second;
    }();
    if (first_cvt) {
        log::log_warning(
            "[decode] sws_rgba src_fmt=%d (%dx%d) flags=%s => sws_cs_id=%d srcRange=%d dstRange=1: "
            "resolved per-file spec matrix=%s range=%s (tags+probe)",
            static_cast<int>(cvt->format), cvt->width, cvt->height,
            scaled ? "scaled" : "unscaled",
            sws_matrix, src_range,
            gpu::color_matrix_name(d.matrix), gpu::color_range_name(d.range));
    }
    sws_scale(sws_ctx_, cvt->data, cvt->linesize, 0, cvt->height, dst_data, dst_linesize);
    convert_ms_ += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - conv_t0).count();
    static auto sws_agg_at = std::chrono::steady_clock::now();
    static int sws_agg_n = 0;
    static double sws_agg_ms = 0.0, sws_max_ms = 0.0;
    static int sws_scaled = 0;
    static int sws_sw = 0, sws_sh = 0, sws_dw = 0, sws_dh = 0;
    const double cvt_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - conv_t0).count();
    ++sws_agg_n;
    sws_agg_ms += cvt_ms;
    sws_max_ms = std::max(sws_max_ms, cvt_ms);
    if (scaled) ++sws_scaled;
    sws_sw = cvt->width; sws_sh = cvt->height;
    sws_dw = out_w; sws_dh = out_h;
    const auto sws_now = std::chrono::steady_clock::now();
    if (sws_agg_n == 1 || sws_now - sws_agg_at >= std::chrono::seconds(1)) {
        sws_agg_at = sws_now;
        log::log_warning(
            "[decode] sws_avg_ms=%.2f sws_max_ms=%.2f n=%d scaled=%d src=%dx%d dst=%dx%d",
            sws_agg_ms / static_cast<double>(sws_agg_n), sws_max_ms, sws_agg_n,
            sws_scaled, sws_sw, sws_sh, sws_dw, sws_dh);
        sws_agg_n = 0;
        sws_agg_ms = sws_max_ms = 0.0;
        sws_scaled = 0;
    }
    av_frame_free(&sw);
    return out;
}

VideoFramePtr SoftDecoder::seek_to_frame(int64_t target, int max_output_dim) {
    if (!demux_ || !demux_->fmt_ctx || demux_->frame_rate <= 0.0) return nullptr;
    DemuxState& d = *demux_;
    set_output_dim(max_output_dim);
    d.refine_last_frame();
    target = d.clamp_target(target);

    const auto us = static_cast<int64_t>(
        std::llround(target / d.frame_rate * static_cast<double>(AV_TIME_BASE)));
    CANVAS_LOG("decode: video seek_to_frame=%lld (%.3fs)", (long long)target,
           target / d.frame_rate);
    const auto st_t0 = std::chrono::steady_clock::now();
    if (avformat_seek_file(d.fmt_ctx, -1, INT64_MIN, us, us, 0) < 0 && us != 0)
        avformat_seek_file(d.fmt_ctx, -1, INT64_MIN, 0, 0, 0);
    d.reset_stream(target);

    VideoFramePtr frame = decode_forward_to(target, kFullResMaxOver);
    const double st_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - st_t0).count();
CANVAS_LOG("vdecode container-seek target=%lld ms=%.2f ok=%d",
           static_cast<long long>(target), st_ms, frame != nullptr);
    if (!frame) return nullptr;
    d.next_frame = frame->frame_number + 1;
    return frame;
}

VideoFramePtr SoftDecoder::decode_to_frame(int64_t target, int max_output_dim) {
    if (!demux_ || !demux_->codec_ctx || demux_->frame_rate <= 0.0) return nullptr;
    DemuxState& d = *demux_;
    set_output_dim(max_output_dim);
    d.refine_last_frame();

    if (d.last_frame > 0 && target > d.last_frame) {
        if (hold_rgba_ && hold_rgba_src_ == d.last_frame && hold_rgba_dim_ == out_max_dim_)
            return hold_rgba_;
        target = d.last_frame;
    } else {
        target = d.clamp_target(target);
    }

    const auto dbg_start = std::chrono::steady_clock::now();
    const bool dbg_hw = d.hw_pix_fmt != AV_PIX_FMT_NONE;
    VideoFramePtr dbg_out = nullptr;

    if (target >= d.next_frame && target - d.next_frame < 64) {
        const auto seq_t0 = std::chrono::steady_clock::now();
        VideoFramePtr frame = decode_forward_to(target, kFullResMaxOver);
        const double seq_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - seq_t0).count();
        if (frame) {
            d.next_frame = frame->frame_number + 1;
            dbg_out = std::move(frame);
            ++path_seq_;
            path_seq_ms_ += seq_ms;
        }
    }
    if (!dbg_out) {
        const auto seek_t0 = std::chrono::steady_clock::now();
        dbg_out = seek_to_frame_indexed(target, max_output_dim);
        ++path_seeks_;
        path_seek_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - seek_t0).count();
    }

    if (dbg_out) {
        const double dbg_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dbg_start).count();
        CANVAS_LOG("decode: frame %lld took %.2f ms hw=%d", (long long)dbg_out->frame_number,
               dbg_ms, (int)dbg_hw);
        if (d.last_frame > 0 && dbg_out->frame_number >= d.last_frame) {
            hold_rgba_ = dbg_out;
            hold_rgba_src_ = dbg_out->frame_number;
            hold_rgba_dim_ = out_max_dim_;
        }
    }
    return dbg_out;
}

VideoFramePtr SoftDecoder::seek_to_frame_indexed(int64_t target, int max_output_dim) {
    if (!demux_) return nullptr;
    DemuxState& d = *demux_;
    set_output_dim(max_output_dim);
    d.refine_last_frame();
    target = d.clamp_target(target);

    const IframeEntry* entry = d.iframe_at_or_before(target);
    if (!entry) {
        CANVAS_LOG("video_decoder: seek_to_frame_indexed NO IFRAME for %lld, fallback to container seek",
               (long long)target);
        return seek_to_frame(target, max_output_dim);
    }

    const int max_over = max_output_dim > 0 ? kPreviewMaxOver : kFullResMaxOver;
    const auto idx_t0 = std::chrono::steady_clock::now();
    d.container_seek_seconds(entry->pts_seconds);
    VideoFramePtr frame = decode_forward_to(target, max_over);
    const double idx_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - idx_t0).count();
    CANVAS_LOG(
                       "vdecode scrub indexed-seek target=%lld iframe=%lld gop_secs=%.3f max=%d ms=%.2f ok=%d",
                       (long long)target, (long long)entry->frame,
                       entry->pts_seconds, max_over, idx_ms, frame != nullptr);
    if (!frame) return nullptr;
    d.next_frame = frame->frame_number + 1;
    return frame;
}

}
