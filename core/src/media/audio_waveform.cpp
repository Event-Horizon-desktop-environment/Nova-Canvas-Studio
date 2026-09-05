#include "canvas/core/media/audio_waveform.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace canvas::core {

namespace {

std::string av_err_string(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets) {
    return reduce_waveform(src, out_buckets, 0.0, 1.0);
}

AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets,
                              double lo_frac, double hi_frac) {
    AudioWaveform out;
    out.buckets = out_buckets;
    out.duration_seconds = src.duration_seconds;
    if (out_buckets == 0 || src.buckets == 0 || src.peak.size() < src.buckets) {
        out.peak.assign(out_buckets, 0.0f);
        out.rms.assign(out_buckets, 0.0f);
        return out;
    }
    // Whole-file fallback for degenerate/out-of-band ranges; zero-width ranges
    // still render (they pin to the same slice rather than a blank preview).
    if (!(lo_frac < hi_frac)) { lo_frac = 0.0; hi_frac = 1.0; }
    if (lo_frac >= 1.0 || hi_frac <= 0.0) { lo_frac = 0.0; hi_frac = 1.0; }
    const double lo = std::clamp(lo_frac, 0.0, 1.0);
    const double hi = std::clamp(hi_frac, 0.0, 1.0);
    const double span = hi - lo;
    out.peak.assign(out_buckets, 0.0f);
    out.rms.assign(out_buckets, 0.0f);
    for (std::size_t b = 0; b < out_buckets; ++b) {
        const double f0 = lo + span * static_cast<double>(b) / static_cast<double>(out_buckets);
        const double f1 = lo + span * static_cast<double>(b + 1) / static_cast<double>(out_buckets);
        std::size_t s0 = static_cast<std::size_t>(std::floor(f0 * static_cast<double>(src.buckets)));
        std::size_t s1 = static_cast<std::size_t>(std::ceil(f1 * static_cast<double>(src.buckets)));
        if (s0 >= src.buckets) s0 = src.buckets - 1;  // clamp the very last bucket
        if (s1 <= s0) s1 = std::min(src.buckets, s0 + 1);  // guarantee >= 1 sample
        if (s1 > src.buckets) s1 = src.buckets;
        float peak = 0.0f;
        double sum = 0.0;
        std::size_t n = 0;
        for (std::size_t s = s0; s < s1; ++s) {
            peak = std::max(peak, src.peak[s]);
            sum += src.rms[s];
            ++n;
        }
        if (n) {
            out.peak[b] = std::min(1.0f, peak);
            out.rms[b] = std::min(1.0f, static_cast<float>(sum / n));
        }
    }
    return out;
}

bool decode_audio_waveform(const std::string& path, std::size_t buckets, AudioWaveform* out,
                           std::string* error) {
    if (!out || buckets == 0) {
        if (error) *error = "invalid arguments";
        return false;
    }
    *out = AudioWaveform{};
    out->buckets = buckets;
    out->peak.assign(buckets, 0.0f);
    out->rms.assign(buckets, 0.0f);

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
        if (error) *error = "could not open file: " + path;
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        if (error) *error = "could not read stream info";
        avformat_close_input(&fmt);
        return false;
    }

    const int64_t total_seconds = fmt->duration > 0 ? fmt->duration / AV_TIME_BASE : 0;
    out->duration_seconds = total_seconds > 0 ? static_cast<double>(total_seconds) : 0.0;

    const AVCodec* codec = nullptr;
    AVStream* stream = nullptr;
    int audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (audio_index < 0 || !codec) {
        if (error) *error = "no audio stream found";
        avformat_close_input(&fmt);
        return false;
    }
    stream = fmt->streams[audio_index];

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        if (error) *error = "could not allocate codec context";
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0 ||
        avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        if (error) *error = "could not open audio decoder";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt);
        return false;
    }

    const int sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 48000;
    const int out_channels = 1;

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, sample_rate,
                            &codec_ctx->ch_layout, codec_ctx->sample_fmt, sample_rate, 0, nullptr) < 0 ||
        !swr || swr_init(swr) < 0) {
        if (error) *error = "could not init swresample";
        if (swr) swr_free(&swr);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt);
        return false;
    }

    std::vector<float> bucket_sum_sq(buckets, 0.0f);
    std::vector<std::size_t> bucket_count(buckets, 0);
    const double rate = static_cast<double>(sample_rate);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = true;

    while (ok && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audio_index && avcodec_send_packet(codec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                if (frame->nb_samples <= 0 || !frame->extended_data[0]) continue;
                const double frame_t0 = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                            ? frame->best_effort_timestamp * av_q2d(stream->time_base)
                                            : 0.0;
                std::vector<float> buf(static_cast<std::size_t>(frame->nb_samples), 0.0f);
                uint8_t* out_data = reinterpret_cast<uint8_t*>(buf.data());
                const int got = swr_convert(swr, &out_data, frame->nb_samples,
                                            const_cast<const uint8_t**>(frame->extended_data),
                                            frame->nb_samples);
                const int n = std::max(0, got);
                for (int i = 0; i < n; ++i) {
                    const double t = frame_t0 + static_cast<double>(i) / rate;
                    std::size_t bi = 0;
                    if (total_seconds > 0) {
                        bi = static_cast<std::size_t>((t / static_cast<double>(total_seconds)) * buckets);
                    } else {
                        bi = static_cast<std::size_t>(((i + n) / static_cast<double>(n)) * buckets);
                    }
                    bi = std::min(buckets - 1, bi);
                    const float a = buf[static_cast<std::size_t>(i)] *
                                    static_cast<float>(out_channels);
                    out->peak[bi] = std::max(out->peak[bi], std::fabs(a));
                    bucket_sum_sq[bi] += a * a;
                    bucket_count[bi] += 1;
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    swr_free(&swr);
    avformat_close_input(&fmt);

    const double inv_channels = 1.0 / out_channels;
    for (std::size_t i = 0; i < buckets; ++i) {
        out->peak[i] = std::min(1.0f, out->peak[i] * static_cast<float>(inv_channels));
        out->rms[i] = bucket_count[i] ? std::sqrt(bucket_sum_sq[i] / bucket_count[i]) : 0.0f;
        out->rms[i] = std::min(1.0f, out->rms[i]);
    }
    return ok;
}

}  // namespace canvas::core
