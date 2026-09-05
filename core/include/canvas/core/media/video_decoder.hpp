#pragma once

#include "canvas/core/media/frame.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace canvas::core {

// One entry in a media file's keyframe (I-frame) index. Built once at open time
// by walking the container packet stream and recording every keyframe's byte
// position and presentation time. Scrubbing uses this to seek straight to the
// nearest I-frame at-or-before the target and know exactly how far the
// decode-forward tail is — no per-seek container search, so random scrub seeks
// cost only the single intervening Group of Pictures instead of a demuxer scan.
struct IframeEntry {
    int64_t packet_pos = 0;   // file byte offset of the keyframe packet
    double pts_seconds = 0.0; // presentation time (seconds) of the keyframe
    int64_t frame = 0;        // frame number of the keyframe
};

// Process-wide I-frame index cache, keyed by media path. Populated by
// background builder threads (see build_iframe_index) and read by decoders on
// the playback thread. Owned here (not on any VideoDecoder) so a background
// build that outlives a decoder still lands safely in the cache.
inline std::mutex s_iframe_mtx;
inline std::map<std::string, std::shared_ptr<const std::vector<IframeEntry>>> s_iframe_cache;
inline std::set<std::string> s_iframe_inflight;

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Opens `path` for decoding. If `hw_device_ctx` is non-null the decoder
    // attempts hardware-accelerated decoding on that (shared) device, falling
    // back to software transparently if the codec/GPU don't support it. Frames
    // are always returned in CPU-accessible RGBA via `VideoFrame`.
    bool open(const std::string& path, std::string* error = nullptr,
              const AVBufferRef* hw_device_ctx = nullptr);
    void close();

    [[nodiscard]] bool is_open() const { return fmt_ctx_ != nullptr; }
    [[nodiscard]] bool is_hardware() const { return hw_avail_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int nb_streams() const { return fmt_ctx_ ? fmt_ctx_->nb_streams : 0; }
    [[nodiscard]] int video_stream_index() const { return video_stream_; }
    // Container stream census for diagnostics: total stream count, which stream
    // av_find_best_stream picked as THE video stream, and every video stream in
    // the container (index:codec:WxH). Lets logs answer "does this file have
    // multiple video streams?" — av_find_best_stream only ever uses one.
    [[nodiscard]] std::string video_stream_summary() const;
    [[nodiscard]] double frame_rate() const { return frame_rate_; }
    [[nodiscard]] double duration_seconds() const { return duration_seconds_; }
    [[nodiscard]] int64_t total_frames() const { return total_frames_; }
    [[nodiscard]] int64_t current_frame() const { return next_frame_; }

    VideoFramePtr decode_next();
    void set_output_dim(int max_output_dim);

    // Returns the frame at `target_frame`, decoding forward sequentially from
    // the current position when that is cheap (target >= next_frame_), so
    // smooth playback doesn't re-seek (and re-decode from a keyframe) on every
    // frame. Falls back to seek_to_frame() for backwards/random access.
    //
    // `max_output_dim` (0 = full resolution) caps the longest output edge of
    // the returned RGBA frame. The decoder still decodes at the source
    // resolution (HEVC has no cheap decode-time downscale), but the scaler
    // shrinks to a small RGBA during conversion. For scrubbing, where every
    // keyframe->target seek converts a whole Group of Pictures at full res, this
    // drops the per-frame convert cost and buffer traffic roughly quadratically.
    // Playback and thumbnails should keep the default (full resolution) unless a
    // low-res preview is explicitly wanted.
    VideoFramePtr decode_to_frame(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr seek_to_frame(int64_t target_frame, int max_output_dim = 0);

    // Keyframe (I-frame) index of the media file, built lazily on first access.
    // Empty until build_iframe_index() runs. Scrubbing seeks via this to avoid
    // per-seek container searches; the decode-forward distance is the number of
    // frames from the chosen I-frame to the target.
    [[nodiscard]] bool has_iframe_index() const {
        if (path_.empty()) return false;
        std::lock_guard<std::mutex> lk(s_iframe_mtx);
        const auto it = s_iframe_cache.find(path_);
        return it != s_iframe_cache.end() && it->second && it->second->size() > 1;
    }
    // Builds the I-frame index on demand (a single packet walk over the stream).
    void build_iframe_index();
    // Returns the index entry for the I-frame nearest at-or-before `target`, or
    // nullptr if the index isn't built / `target` is before the first I-frame.
    // The decoder's next-frame position does NOT change here.
    [[nodiscard]] const IframeEntry* iframe_at_or_before(int64_t target) const;
    // Frame-accurate seek that first jumps to the I-frame from the index (when
    // available) then decode-forwards to `target` within that single GOP. Equal
    // in result to seek_to_frame() but avoids the container search.
    VideoFramePtr seek_to_frame_indexed(int64_t target, int max_output_dim = 0);

    // Decodes forward to `target` and, when hardware decoding is active,
    // returns a *borrowed* pointer to the raw hardware frame (device NV12 /
    // AV_PIX_FMT_CUDA) WITHOUT downloading it to the CPU or converting to RGBA.
    // The returned frame is valid only until the next decode call on this
    // decoder and must not be freed by the caller. Returns nullptr when
    // hardware decode is inactive/unavailable, so the caller falls back to the
    // CPU RGBA path. Used by the exporter's GPU composite fast path.
    //
    // `max_over` bounds how many intermediate frames are fast-overed before the
    // target (0 = exact). When exceeded, the nearest frame reached is returned
    // as an approximate teaser so sparse-keyframe scrub previews stay fast;
    // callers that need the exact target (export/scrub-commit) must pass 0.
    const AVFrame* decode_to_hw(int64_t target_frame, int max_over = 0);

    // Keyframe-anchored hardware decode: seeks the container to the I-frame
    // nearest at-or-before `target_frame` (between calls it decodes forward
    // within that single GOP), then returns the borrowed device frame like
    // decode_to_hw. Mirrors seek_to_frame_indexed() for the GPU NV12 path, so a
    // scrub jump only pays for one Group of Pictures instead of walking from the
    // decoder's current position. `max_over` bounds the within-GOP fast-over
    // like decode_to_hw. Returns nullptr when hardware decode is inactive or the
    // target is before the first I-frame.
    const AVFrame* decode_to_hw_indexed(int64_t target_frame, int max_over = 0);

    // Fast-over budget for scrub previews: the maximum number of intermediate
    // GOP frames a preview decode (either CPU or GPU) will step through before
    // returning a lower-cost approximate frame. Larger = more accurate but
    // slower stalls on sparse-keyframe media; 0 = exact/uncapped.
    static const int kPreviewMaxOver = 1200;

    // ---- Audio ----
    [[nodiscard]] bool has_audio() const { return audio_stream_ >= 0; }
    [[nodiscard]] int audio_sample_rate() const { return audio_sample_rate_; }
    [[nodiscard]] int audio_channels() const { return audio_channels_; }
    // Decodes up to `max_frames` interleaved float-PCM frames starting at
    // `start_sample`, resampled to `out_sample_rate` when it differs from the
    // source rate. Sequential decode is used when cheap; otherwise it reseeks.
    AudioChunkPtr decode_audio(int64_t start_sample, int max_frames, int out_sample_rate);
    // Re-positions the audio decoder to `start_sample` (given in `sample_rate`
    // units, matching the output/playback rate).
    void seek_audio(int64_t start_sample, int sample_rate);

private:
    void reset_stream_state(int64_t resume_frame);
    VideoFramePtr make_rgba_frame(const AVFrame* src, int64_t ticks, double seconds, int64_t number);
    // Decodes forward from the current position until it produces `target`,
    // fast-overs (no RGBA conversion, no GPU->CPU copy) every intermediate
    // Group-of-Picture frame, and converts only the target frame to RGBA.
    // Returns nullptr on EOF. Backs fast scrub previews. `max_over` bounds the
    // number of fast-overs (0 = uncapped/exact); when exceeded, the nearest
    // decoded frame is returned instead so sparse-keyframe scrub previews stay
    // instant.
    VideoFramePtr decode_forward_to(int64_t target, int max_over = 0);
    // Positions the demuxer to `target_seconds` via avformat_seek_file without
    // decoding (used by the indexed seek path).
    void container_seek_seconds(double target_seconds);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVFrame* av_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int hw_pix_fmt_ = AV_PIX_FMT_NONE;
    bool hw_avail_ = false;

    int video_stream_ = -1;
    AVRational stream_tb_{0, 1};
    int width_ = 0;
    int height_ = 0;
    double frame_rate_ = 0.0;
    double duration_seconds_ = 0.0;
    int64_t total_frames_ = -1;
    int64_t next_frame_ = 0;
    bool draining_ = false;
    int out_max_dim_ = 0;

    // Keyframe index (see IframeEntry). Built lazily on background threads into a
    // process-wide cache keyed by path (s_iframe_cache); never on the
    // decode/playback thread. Seeks fall back to plain container access until
    // an index is ready. Owned outside the decoder so background builds safely
    // outlive decoder teardown.
    std::string path_;

    // Audio stream decode state. The audio demuxer (audio_fmt_ctx_) is a
    // *separate* AVFormatContext from fmt_ctx_ so that video lookahead and
    // audio decoding each have an independent demux/seek position. Sharing one
    // container caused each reader to discard the other's packets and to issue
    // mutual full-container avformat_seek_file on every video frame (0.5 fps).
    int audio_stream_ = -1;
    AVFormatContext* audio_fmt_ctx_ = nullptr;
    AVCodecContext* audio_codec_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    AVFrame* audio_frame_ = nullptr;
    AVPacket* audio_packet_ = nullptr;
    int audio_sample_rate_ = 0;
    int audio_channels_ = 0;
    int64_t audio_next_sample_ = 0;
    int64_t audio_samples_total_ = 0;
    AVRational audio_tb_{0, 1};
};

}
