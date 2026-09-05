#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

namespace canvas::core {

struct AudioDecoder::Impl {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    int stream_index = -1;
    int sample_rate = 0;
    int channels = 0;
    AVRational tb{0, 1};

    // Position (in *output* sample units = seconds * out_sample_rate) of the
    // next sample the decoder will produce. Used to decide whether to re-seek.
    int64_t next_sample = 0;
    int current_out_rate = 48000;

    // Persistent decode buffer: `decoded` holds interleaved float PCM produced but
    // not yet handed to the caller; `decoded_at` is the absolute output-sample
    // position of decoded[0]. Leftover samples carry between calls so sequential
    // forward playback decodes continuously instead of re-clipping per call.
    std::vector<float> decoded;
    int64_t decoded_at = 0;

    ~Impl() {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (swr_ctx) swr_free(&swr_ctx);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
    }
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::has_audio() const { return impl_->stream_index >= 0; }
int AudioDecoder::source_sample_rate() const { return impl_->sample_rate; }
int AudioDecoder::source_channels() const { return impl_->channels; }

double AudioDecoder::duration_seconds() const {
    if (!impl_->fmt_ctx || impl_->stream_index < 0) return 0.0;
    if (impl_->fmt_ctx->duration > 0)
        return static_cast<double>(impl_->fmt_ctx->duration) / static_cast<double>(AV_TIME_BASE);
    if (impl_->stream_index < static_cast<int>(impl_->fmt_ctx->nb_streams)) {
        const AVStream* s = impl_->fmt_ctx->streams[impl_->stream_index];
        if (s && s->duration > 0) return s->duration * av_q2d(s->time_base);
    }
    return 0.0;
}

bool AudioDecoder::open(const std::string& path) {
    close();
    CANVAS_LOG("audio_decoder: open '%s'", path.c_str());

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        log::log_error("audio_decoder: open FAILED avformat_open_input '%s'", path.c_str());
        return false;
    }
    impl_->fmt_ctx = fmt;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        log::log_error("audio_decoder: open FAILED avformat_find_stream_info '%s'", path.c_str());
        close();
        return false;
    }

    const int a = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (a < 0) {
        log::log_error("audio_decoder: open FAILED no audio stream in '%s'", path.c_str());
        close();
        return false;
    }
    const AVStream* ast = fmt->streams[a];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    if (!ac) {
        log::log_error("audio_decoder: open FAILED no decoder for codec_id=%d in '%s'",
                        static_cast<int>(ast->codecpar->codec_id), path.c_str());
        close();
        return false;
    }
    AVCodecContext* cc = avcodec_alloc_context3(ac);
    if (!cc || avcodec_parameters_to_context(cc, ast->codecpar) < 0 ||
        avcodec_open2(cc, ac, nullptr) < 0) {
        log::log_error("audio_decoder: open FAILED codec setup (alloc=%p params=%d open=%d) '%s'",
                        (void*)cc, cc ? 1 : 0, 1, path.c_str());
        if (cc) avcodec_free_context(&cc);
        close();
        return false;
    }
    impl_->codec_ctx = cc;
    impl_->stream_index = a;
    impl_->tb = ast->time_base;
    impl_->sample_rate = cc->sample_rate > 0 ? cc->sample_rate : 48000;
    int nch = static_cast<int>(cc->ch_layout.nb_channels);
    if (nch <= 0) nch = static_cast<int>(ast->codecpar->ch_layout.nb_channels);
    if (nch <= 0) nch = 2;
    impl_->channels = nch;
    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    impl_->next_sample = 0;
    impl_->current_out_rate = 48000;
    impl_->decoded.clear();
    impl_->decoded_at = 0;
    const bool ok = impl_->frame && impl_->packet;
    if (ok)
        CANVAS_LOG("audio_decoder: open OK stream=%d rate=%d ch=%d tb=%d/%d",
               a, impl_->sample_rate, impl_->channels, impl_->tb.num, impl_->tb.den);
    else
        log::log_error("audio_decoder: open FAILED alloc frame/packet");
    return ok;
}

void AudioDecoder::close() {
    if (impl_->fmt_ctx)
        CANVAS_LOG("audio_decoder: close (stream=%d buffered=%lld decoded_at=%lld)",
               impl_->stream_index,
               (long long)(impl_->decoded.size() / std::max<std::size_t>(1, impl_->channels)),
               (long long)impl_->decoded_at);
    if (impl_->packet) av_packet_free(&impl_->packet);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->swr_ctx) swr_free(&impl_->swr_ctx);
    if (impl_->codec_ctx) avcodec_free_context(&impl_->codec_ctx);
    if (impl_->fmt_ctx) avformat_close_input(&impl_->fmt_ctx);
    impl_->packet = nullptr;
    impl_->frame = nullptr;
    impl_->swr_ctx = nullptr;
    impl_->codec_ctx = nullptr;
    impl_->fmt_ctx = nullptr;
    impl_->stream_index = -1;
    impl_->sample_rate = 0;
    impl_->channels = 0;
    impl_->tb = {0, 1};
    impl_->next_sample = 0;
    impl_->decoded.clear();
    impl_->decoded_at = 0;
}

void AudioDecoder::seek(const int64_t start_sample, const int out_sample_rate) {
    if (!impl_->fmt_ctx || impl_->stream_index < 0) {
        CANVAS_LOG("audio_decoder: seek SKIPPED (no fmt_ctx or stream_index=%d)", impl_->stream_index);
        return;
    }
    int64_t us = 0;
    if (out_sample_rate > 0)
    us = static_cast<int64_t>(
        std::llround(start_sample / static_cast<double>(out_sample_rate) * AV_TIME_BASE));
    CANVAS_LOG("audio_decoder: seek sample=%lld us=%lld rate=%d",
           (long long)start_sample, (long long)us, out_sample_rate);
    // Seek by file-wide timestamp (stream index = -1), matching how the video
    // decoder seeks. Indexing the audio stream specifically on uncompressed PCM
    // in Matroska lands at the wrong position and yields silence for far seeks
    // (observed: 100s seek returned 800 silent frames), though sequential
    // forward decode of the same region is correct.
    const int seek_ok = avformat_seek_file(impl_->fmt_ctx, -1, INT64_MIN, us, us, 0);
    if (seek_ok < 0 && us != 0) {
        CANVAS_LOG("audio_decoder: seek primary FAILED ret=%d, retrying to 0", seek_ok);
        avformat_seek_file(impl_->fmt_ctx, -1, INT64_MIN, 0, 0, 0);
    }
    if (impl_->codec_ctx) avcodec_flush_buffers(impl_->codec_ctx);
    if (impl_->swr_ctx) {
        swr_free(&impl_->swr_ctx);
        impl_->swr_ctx = nullptr;
    }
    impl_->next_sample = start_sample;
    impl_->current_out_rate = out_sample_rate;
    impl_->decoded.clear();
    // Anchor the carry buffer at the new target: if decoded_at were left 0 the next
    // sequential call would compute target >> decoded_end, re-seek to the same
    // place and serve the same first slice forever (repeating audio instead of
    // advancing).
    impl_->decoded_at = start_sample;
}

void AudioDecoder::reset() {
    CANVAS_LOG("audio_decoder: reset (next_sample=%lld buffered=%lld decoded_at=%lld)",
           (long long)impl_->next_sample,
           (long long)(impl_->decoded.size() / std::max<std::size_t>(1, impl_->channels)),
           (long long)impl_->decoded_at);
    // FULL rewind to the stream start, not just a codec flush. reset() re-arms
    // playback at a NEW playhead and the next decode() must serve whatever region
    // is asked for. Flushing only the codec leaves the DEMUXER where the previous
    // run ended, so an in-window decode right after the reset would silently
    // return the previous run's tail while labeling it with the new position.
    // seek(0) is the one container seek proven reliable for audio demuxing.
    seek(0, 48000);
}

namespace {
bool ensure_swr(SwrContext*& swr, const AVCodecContext* c, const int out_rate,
                const int channels) {
    if (swr) return true;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels);
    SwrContext* s = nullptr;
    const int alloc =
        swr_alloc_set_opts2(&s, &out_layout, AV_SAMPLE_FMT_FLT, out_rate, &c->ch_layout,
                            c->sample_fmt, c->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (alloc < 0 || !s || swr_init(s) < 0) {
        if (s) swr_free(&s);
        ::canvas::core::log::log_error(
            "audio_decoder: ensure_swr FAILED alloc=%d swr=%p in_rate=%d out_rate=%d in_ch=%d out_ch=%d",
            alloc, (void*)s, c->sample_rate, out_rate, c->ch_layout.nb_channels, channels);
        return false;
    }
    CANVAS_LOG("audio_decoder: ensure_swr OK in_rate=%d out_rate=%d in_ch=%d out_ch=%d in_fmt=%d",
           c->sample_rate, out_rate, c->ch_layout.nb_channels, channels, c->sample_fmt);
    swr = s;
    return true;
}
}  // namespace

AudioChunkPtr AudioDecoder::decode(const int64_t start_sample, const int max_frames,
                                   const int out_sample_rate) {
    if (!impl_->codec_ctx || impl_->stream_index < 0 || out_sample_rate <= 0 || max_frames <= 0) {
        log::log_error("audio_decoder: decode BAD_ARGS codec=%p stream=%d rate=%d max=%d",
                        (void*)impl_->codec_ctx, impl_->stream_index, out_sample_rate, max_frames);
        return nullptr;
    }
    const int64_t target = std::max<int64_t>(0, start_sample);
    const int64_t ch = impl_->channels;

    // Resample rate / channel config changed -> rebuild the resampler and rewind
    // to the stream start (the reliable container seek, see resync below) so the
    // walk re-enters the stream.
    if (impl_->current_out_rate != out_sample_rate) {
        CANVAS_LOG("audio_decoder: decode rate CHANGE %d->%d, rewinding",
               impl_->current_out_rate, out_sample_rate);
        seek(0, out_sample_rate);
    }

    const int64_t buffered =
        static_cast<int64_t>(impl_->decoded.size() / static_cast<std::size_t>(ch));
    // Out-of-window request: the carried buffer plus one decode work unit can't
    // serve it. Contiguous non-repeating slicing (playback forward, export) asks
    // for positions inside, or one work unit past, decoded_at+buffered, so never
    // reaches here. Two request shapes do, and both restart from the stream start
    // and DECODE-AND-DISCARD forward to the target:
    //   * backward jumps (real scrubs, a re-anchored playhead rewind)
    //   * forward jumps to a region the walk hasn't reached (a trimmed head on a
    //     reset decoder, or a far scrub grab)
    // Arbitrary forward container seeks are deliberately NOT used: for audio
    // demuxing avformat_seek_file mis-lands in practice (observed at the file
    // START — PCM in Matroska — and past the file END — AAC in MP4), yielding
    // the wrong region or silence. A sequential walk from the start is always
    // the exact stream content; at ~100x realtime even a multi-minute head costs
    // only a few hundred milliseconds.
    const bool should_resync =
        target < impl_->decoded_at - std::max<int64_t>(max_frames, buffered) ||
        target >= impl_->decoded_at + std::max<int64_t>(buffered, max_frames);
    if (should_resync) {
        if (log::enabled())
            CANVAS_LOG("audio decode: RESYNC target=%lld decoded_at=%lld buffered=%lld rate=%d",
                   static_cast<long long>(target), static_cast<long long>(impl_->decoded_at),
                   static_cast<long long>(buffered), out_sample_rate);
        seek(0, out_sample_rate);
    }

    if (!ensure_swr(impl_->swr_ctx, impl_->codec_ctx, out_sample_rate, impl_->channels)) {
        log::log_error("audio_decoder: decode ensure_swr FAILED target=%lld", (long long)target);
        return nullptr;
    }

    // Shared forward-decode driver: pull packets, decode, resample, append PCM to
    // the buffer until at least `min_frames` are buffered (or the stream ends).
    // The codec receive/send state machine lives across calls of this lambda so
    // a resync walk and the final serve share one contiguous decode pass.
    bool flushed = false;
    bool eof = false;
    int ret = AVERROR(EAGAIN);
    const auto fill_in = [&](int64_t min_frames) {
        if (eof) return;
        while (static_cast<int64_t>(impl_->decoded.size() / static_cast<std::size_t>(ch)) <
               min_frames) {
            if (ret == 0 || ret == AVERROR(EAGAIN)) {
                ret = avcodec_receive_frame(impl_->codec_ctx, impl_->frame);
            }
            if (ret == 0) {
                const int in_samples = impl_->frame->nb_samples;
                if (in_samples > 0 && impl_->swr_ctx) {
                    const int max_out =
                        static_cast<int>(swr_get_out_samples(impl_->swr_ctx, in_samples));
                    std::vector<float> tmp(static_cast<std::size_t>(max_out) *
                                           static_cast<std::size_t>(ch));
                    uint8_t* tmp_ptr[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
                    const int got =
                        swr_convert(impl_->swr_ctx, tmp_ptr, max_out,
                                    const_cast<const uint8_t**>(impl_->frame->extended_data),
                                    in_samples);
                    if (got > 0)
                        impl_->decoded.insert(impl_->decoded.end(), tmp.begin(),
                                              tmp.begin() + static_cast<std::ptrdiff_t>(got) * ch);
                }
                av_frame_unref(impl_->frame);
                continue;
            }
            if (ret == AVERROR_EOF) {
                // Drain any residual resampler delay once.
                if (!flushed && impl_->swr_ctx) {
                    swr_convert(impl_->swr_ctx, nullptr, 0, nullptr, 0);
                    flushed = true;
                }
                eof = true;
                return;
            }
            if (ret != AVERROR(EAGAIN)) return;

            // Read the next audio packet.
            for (;;) {
                const int r = av_read_frame(impl_->fmt_ctx, impl_->packet);
                if (r < 0) {
                    CANVAS_LOG("audio_decoder: decode packet EOF ret=%d, flushing codec", r);
                    if (!flushed) {
                        avcodec_send_packet(impl_->codec_ctx, nullptr);
                        flushed = true;
                    }
                    eof = true;
                    return;
                }
                if (impl_->packet->stream_index != impl_->stream_index) {
                    av_packet_unref(impl_->packet);
                    continue;
                }
                avcodec_send_packet(impl_->codec_ctx, impl_->packet);
                av_packet_unref(impl_->packet);
                break;
            }
            ret = AVERROR(EAGAIN);
        }
    };

    // After a resync, walk forward and DISCARD everything before the requested
    // position, in bounded work units so no giant intermediate buffer is held.
    if (target > impl_->decoded_at) {
        while (impl_->decoded_at < target && !eof) {
            const int64_t still_need = target - impl_->decoded_at;
            fill_in(std::min<int64_t>(still_need, 8192));
            const int64_t avail =
                static_cast<int64_t>(impl_->decoded.size() / static_cast<std::size_t>(ch));
            const int64_t drop = std::min<int64_t>(still_need, avail);
            if (drop > 0)
                CANVAS_LOG("audio_decoder: decode DISCARD %lld frames (%lld..%lld) to reach target %lld",
                       (long long)drop, (long long)impl_->decoded_at,
                       (long long)(impl_->decoded_at + drop), (long long)target);
            impl_->decoded.erase(impl_->decoded.begin(),
                                 impl_->decoded.begin() + static_cast<std::ptrdiff_t>(drop * ch));
            impl_->decoded_at += drop;
            if (drop == 0) break;  // stalled at EOF
        }
    }

    // Decode forward, appending to the persistent buffer until at least `max_frames`
    // are ready to serve. Leftover carries to the next call so sequential
    // playback never stops or re-seeks.
    fill_in(max_frames);

    const int64_t available =
        static_cast<int64_t>(impl_->decoded.size() / static_cast<std::size_t>(ch));
    const int64_t serve = std::min<int64_t>(max_frames, available);
    if (serve <= 0) {
        CANVAS_LOG("audio_decoder: decode EMPTY serve=0 available=%lld target=%lld eof=%d",
               (long long)available, (long long)target, (int)eof);
        impl_->next_sample = impl_->decoded_at;
        return nullptr;
    }

    auto chunk = std::make_shared<AudioChunk>();
    chunk->start_sample = impl_->decoded_at;
    chunk->sample_rate = out_sample_rate;
    chunk->channels = static_cast<int>(ch);
    chunk->samples.assign(
        impl_->decoded.begin(),
        impl_->decoded.begin() + static_cast<std::ptrdiff_t>(serve * ch));
    impl_->decoded.erase(impl_->decoded.begin(),
                         impl_->decoded.begin() + static_cast<std::ptrdiff_t>(serve * ch));
    impl_->decoded_at += serve;
    impl_->next_sample = impl_->decoded_at;
    CANVAS_LOG("audio_decoder: decode OK target=%lld served=%lld buffered=%lld sample_rate=%d",
           (long long)target, (long long)serve,
           (long long)(impl_->decoded.size() / static_cast<std::size_t>(ch)), out_sample_rate);
    return chunk;
}

}  // namespace canvas::core
