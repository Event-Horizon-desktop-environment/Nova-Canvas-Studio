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

// One entry in a media file's keyframe (I-frame) index. Built once by walking
// the container packet stream and recording each keyframe's byte position and
// presentation time. Scrubbing uses it to seek straight to the I-frame at-or-
// before the target and know exactly how long the decode-forward tail is — no
// per-seek container search, so random seeks cost one Group of Pictures instead
// of a demuxer scan.
struct IframeEntry {
    int64_t packet_pos = 0;   // file byte offset of the keyframe packet
    double pts_seconds = 0.0; // presentation time (seconds) of the keyframe
    int64_t frame = 0;        // frame number of the keyframe
};

// Process-wide I-frame index cache, keyed by media path. Populated by
// background builder threads, read by decoders on the playback thread. Owned
// here (not on any VideoDecoder) so a build that outlives a decoder still lands
// safely in the cache.
inline std::mutex s_iframe_mtx;
inline std::map<std::string, std::shared_ptr<const std::vector<IframeEntry>>> s_iframe_cache;
inline std::set<std::string> s_iframe_inflight;

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    // Movable: a prepared decode session (e.g. a transition pre-render parked at
    // the incoming clip's head) can be handed to another owner without tearing
    // down and re-opening the FFmpeg contexts. Move-assign closes whatever this
    // decoder currently holds, steals the source's contexts/state, and leaves the
    // source closed and reusable.
    VideoDecoder(VideoDecoder&& o) noexcept;
    VideoDecoder& operator=(VideoDecoder&& o) noexcept;
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
    // True when the container's best video stream is an embedded still rather
    // than motion video — album art (mp3/m4a/ogg/flac cover art is exposed as
    // a one-frame attached-picture or single-packet stream). Import uses this
    // to classify such files as audio-only so the pool/show preview shows a
    // spectrum, not the artwork. Ogg/FLAC art has no attached_pic disposition,
    // so the check also runs a demux-only packet scan (never decodes).
    [[nodiscard]] bool is_still_picture() const;
    // Container stream census for diagnostics: total stream count, which stream
    // av_find_best_stream picked, and every video stream in the container
    // (index:codec:WxH). Answers "does this file have multiple video streams?".
    [[nodiscard]] std::string video_stream_summary() const;
    [[nodiscard]] double frame_rate() const { return frame_rate_; }
    [[nodiscard]] double duration_seconds() const { return duration_seconds_; }
    [[nodiscard]] int64_t total_frames() const { return total_frames_; }
    [[nodiscard]] int64_t current_frame() const { return next_frame_; }
    // Resolved per-file color spec: matrix/range read from codecpar and
    // reconciled against a decoded-luma probe when a `tv`-style tag lies about
    // full-range data. Every frame this decoder produces (RGBA via make_rgba,
    // NV12 via decode_to_hw) is consistent with this spec.
    [[nodiscard]] gpu::ColorSpec color_spec() const { return {matrix_, range_}; }
    // Highest valid target frame index for this stream (inclusive), once known.
    // Returns -1 when the encoded extent is not yet known (no frame decoded and
    // neither the container nor stream duration is available).
    [[nodiscard]] int64_t last_frame() const { return last_frame_; }

    VideoFramePtr decode_next();
    void set_output_dim(int max_output_dim);

    // Per-path access accounting for diagnostics: how frames reached the caller —
    // sequential forward walk (the cheap steady-playback path) vs a keyframe
    // (re)seek. Steady playback must be ~100% sequential; a seek-heavy ratio in
    // the [dec] aggregate means the playhead keeps jumping the sequential window
    // (watermark/commit mismatch) or the cache is cold after every seek.
    struct PathStats {
        std::uint64_t sequential = 0;
        std::uint64_t seeks = 0;
        double sequential_ms = 0.0;
        double seek_ms = 0.0;
        // Fraction of decode_to_frame cost spent in pixel conversion
        // (hw->cpu transfer + swscale). A convert_ms/sequential_ms near 1.0 with
        // a climbing avg decode_ms points at the download/scaler, not the codec.
        double convert_ms = 0.0;
    };
    [[nodiscard]] PathStats path_stats() const;
    PathStats take_path_stats();

    // Returns the frame at `target_frame`, decoding forward sequentially from the
    // current position when that is cheap (target >= next_frame_), so smooth
    // playback doesn't re-seek (and re-decode from a keyframe) on every frame.
    // Falls back to seek_to_frame() for backwards/random access.
    //
    // `max_output_dim` (0 = full resolution) caps the longest output edge of the
    // returned RGBA frame, shrinking in the scaler while the decode stays at
    // source resolution (HEVC has no cheap decode-time downscale). For scrubs,
    // where every seek converts a whole GOP at full res, this cuts the per-frame
    // convert cost and buffer traffic roughly quadratically. Playback keeps the
    // default unless a low-res preview is explicitly wanted.
    VideoFramePtr decode_to_frame(int64_t target_frame, int max_output_dim = 0);
    VideoFramePtr seek_to_frame(int64_t target_frame, int max_output_dim = 0);

    // Keyframe (I-frame) index of the media file, built lazily on first access
// (empty until build_iframe_index() runs). Scrubbing seeks via this to avoid
// per-seek container searches.
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

    // Decodes forward to `target` and, when hardware decoding is active, returns a
    // *borrowed* pointer to the raw hardware frame (NV12 / AV_PIX_FMT_CUDA)
    // without downloading to CPU or converting to RGBA. The frame is valid only
    // until the next decode call on this decoder and must not be freed.
    // Returns nullptr when hardware decode is inactive so the caller falls back
    // to the CPU RGBA path. Used by the exporter's GPU composite fast path.
    //
    // `max_over` bounds how many intermediate frames are fast-overed before the
    // target (0 = exact). When exceeded the nearest frame reached is returned as
    // an approximate teaser so sparse-keyframe scrub previews stay fast; callers
    // that need the exact target (export/scrub-commit) must pass 0.
    const AVFrame* decode_to_hw(int64_t target_frame, int max_over = 0);

    // Keyframe-anchored hardware decode: seeks the container to the I-frame nearest
    // at-or-before `target_frame` (decoding forward within that GOP between
    // calls), then returns the borrowed device frame like decode_to_hw. Mirrors
    // seek_to_frame_indexed() for the GPU NV12 path, so a scrub jump pays one GOP
    // instead of walking from the decoder's current position. `max_over` bounds
    // the within-GOP fast-over like decode_to_hw. nullptr when hardware decode
    // is inactive or the target is before the first I-frame.
    const AVFrame* decode_to_hw_indexed(int64_t target_frame, int max_over = 0);

    // Fast-over budget for scrub previews: the maximum number of intermediate GOP
    // frames a preview decode will step through before returning a lower-cost
    // approximate frame. Larger = more accurate but slower on sparse-keyframe
    // media; 0 = exact/uncapped.
    static const int kPreviewMaxOver = 1200;

    // Fast-over budget for FULL-RES decodes. Steady playback advances 0-1 frames
    // (sequential), so this only binds far-forward seeks into ultra-sparse GOPs
    // (a 19k-frame keyframe interval caused a 78 s walk that garbled audio by
    // letting it pre-roll ahead). A far full-res seek returns the nearest frame
    // as an approximate still instead of blocking the worker for tens of seconds.
    static const int kFullResMaxOver = 4000;

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
    // Clamps `target` into the valid source-frame window [0, last decodeable
    // frame]. When the encoded extent is still unknown (last_frame_ == -1) the
    // target is only floored at 0; the clamp becomes effective as soon as one
    // frame has been decoded or a stream end is observed. This is what stops a
    // scrub/play call over the media's end (e.g. a long music region past the
    // clip) from walking the entire GOP chain to EOF.
    int64_t clamp_target(int64_t target) const;
    // Refines last_frame_ from the stream's own duration when the container
    // duration was unknown at open() time. No-op once a value is already known.
    void refine_last_frame();
    VideoFramePtr make_rgba_frame(const AVFrame* src, int64_t ticks, double seconds, int64_t number);
    // Decodes forward from the current position until it produces `target`,
    // fast-overs (no RGBA conversion, no GPU->CPU copy) every intermediate GOP
    // frame, converts only the target to RGBA. nullptr on EOF. Backs fast scrub
    // previews. `max_over` bounds the fast-overs (0 = uncapped/exact); when
    // exceeded the nearest decoded frame is returned so sparse-keyframe scrub
    // previews stay instant.
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
    // NV12/CUDA engagement tracking: a hardware-configured decoder can still
    // emit SOFTWARE frames when the stream is entered mid-GOP (the AV1 sequence
    // header that primes the NVDEC session lives in an earlier keyframe).
    // hw_engaged_ records the first real CUDA frame; soft_only_ latches when a
    // keyframe re-anchor didn't help so decode_to_hw gives up re-anchoring.
    bool hw_engaged_ = false;
    bool soft_only_ = false;

    int video_stream_ = -1;
    AVRational stream_tb_{0, 1};
    int width_ = 0;
    int height_ = 0;
    double frame_rate_ = 0.0;
    double duration_seconds_ = 0.0;
    int64_t total_frames_ = -1;
    // Resolved per-file color spec (see color_spec()). matrix_/range_ start on
    // the codecpar tags and range_ may be upgraded to Full by the luma probe.
    gpu::ColorMatrix matrix_ = gpu::ColorMatrix::BT709;
    gpu::ColorRange range_ = gpu::ColorRange::Limited;
    int64_t last_frame_ = -1;
    int64_t next_frame_ = 0;
    bool draining_ = false;
    int out_max_dim_ = 0;
    // Frozen-tail hold frames. When a caller targets a frame past the stream's
    // encoded end (a clip whose audio outlives its video, or a far-forward
    // scrub over the media edge), re-seeking and re-decoding the same final
    // frame on every call is ~90ms/frame wasted work that turns the render tail
    // into a 10fps crawl. Instead the last real frame is decoded once and held;
    // any later past-end request serves the cached copy. hold_rgba_ covers the
    // CPU RGBA path, hold_hw_ the GPU NV12 path (a ref-counted copy of the
    // device frame, independent of the reused av_frame_).
    VideoFramePtr hold_rgba_;
    int64_t hold_rgba_src_ = -1;
    int hold_rgba_dim_ = -1;
    AVFrame* hold_hw_ = nullptr;
    int64_t hold_hw_src_ = -1;
    // One-frame sequential lookback for the GPU path. When a transition's
    // fading-out slot runs at a sub-rate (e.g. the B side of a 2:1 clip
    // ratio), consecutive timeline frames can re-target the SAME source
    // frame. The repeat would otherwise be served by decode_to_hw_indexed,
    // whose container seek resets next_frame_ to 0 and forces a full-GOP
    // re-walk (~54ms on 2K60) for every repeated B frame. retain_hw_ keeps a
    // ref-counted copy of the last device frame decode_to_hw actually served
    // (retain_hw_src_ = its frame number); decode_to_hw_indexed serves that
    // exact copy instead of re-seeking when the walk is already parked past
    // it, leaving next_frame_ untouched so the following distinct frame keeps
    // riding the cheap sequential path.
    AVFrame* retain_hw_ = nullptr;
    int64_t retain_hw_src_ = -1;
    // Sequential-walk vs keyframe-seek path accounting (see PathStats).
    std::uint64_t path_seq_ = 0;
    std::uint64_t path_seeks_ = 0;
    double path_seq_ms_ = 0.0;
    double path_seek_ms_ = 0.0;
    double convert_ms_ = 0.0;

    // Keyframe index (see IframeEntry). Built lazily on background threads into a
    // process-wide cache keyed by path (s_iframe_cache); never on the
    // decode/playback thread. Seeks fall back to plain container access until
    // an index is ready. Owned outside the decoder so background builds safely
    // outlive decoder teardown.
    std::string path_;

    // Audio stream decode state, kept in a *separate* AVFormatContext from fmt_ctx_
    // so video lookahead and audio decoding each have an independent demux/seek
    // position. Sharing one container made each reader discard the other's
    // packets and issue mutual full-container seeks on every video frame (0.5fps).
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
