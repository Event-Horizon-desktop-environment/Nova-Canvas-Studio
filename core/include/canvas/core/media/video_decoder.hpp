#pragma once

#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/vaapi/surface.hpp"
#include "canvas/core/media/sw_decode.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <memory>
#include <string>

namespace canvas::core {

// VideoDecoder (see sw_decode.hpp for the software decode path): owns the shared
// DemuxState (demux/decode FFmpeg contexts + stream position), the SoftDecoder
// (software loop + sws RGBA conversion), and the decoder-specific state that
// only the hardware/audio/attribution paths touch: the negotiated HW pixel
// format and engagement latches, the GPU frozen-tail/lookback frames, and the
// separate audio demux/seek domain. Public API is unchanged by the split.
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
    // `gpu_label` (optional) is the resolved physical GPU name of the device
    // (`HwDeviceManager::device_label()`, e.g. "AMD Radeon (Granite Ridge)")
    // and is reported in the [dec] open line so multi-GPU logs name the actual
    // hardware. Null = backend-only attribution ("vaapi"/"sw").
    bool open(const std::string& path, std::string* error = nullptr,
              const AVBufferRef* hw_device_ctx = nullptr,
              const char* gpu_label = nullptr);
    void close();

    [[nodiscard]] bool is_open() const { return demux_state_ && demux_state_->fmt_ctx != nullptr; }
    [[nodiscard]] bool is_hardware() const { return hw_avail_; }
    // Named accelerator this decoder decodes on ("", "vaapi", "cuda", "qsv",
    // "vulkan" — "" = software / no hardware engaged). Same value the [dec]
    // open line reports; exposed so slot-level logs can name the driver too.
    [[nodiscard]] const char* hardware_name() const { return hw_avail_ ? hw_type_name_.c_str() : "sw"; }
    // Physical GPU name this decoder is on ("AMD Radeon (Granite Ridge)"),
    // or "" when software / not resolved.
    [[nodiscard]] const char* gpu_label() const { return hw_gpu_label_.c_str(); }
    [[nodiscard]] int width() const { return demux_state_ ? demux_state_->width : 0; }
    [[nodiscard]] int height() const { return demux_state_ ? demux_state_->height : 0; }
    [[nodiscard]] int nb_streams() const {
        return demux_state_ && demux_state_->fmt_ctx ? demux_state_->fmt_ctx->nb_streams : 0;
    }
    [[nodiscard]] int video_stream_index() const {
        return demux_state_ ? demux_state_->video_stream : -1;
    }
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
    [[nodiscard]] double frame_rate() const { return demux_state_ ? demux_state_->frame_rate : 0.0; }
    [[nodiscard]] double duration_seconds() const {
        return demux_state_ ? demux_state_->duration_seconds : 0.0;
    }
    [[nodiscard]] int64_t total_frames() const {
        return demux_state_ ? demux_state_->total_frames : -1;
    }
    [[nodiscard]] int64_t current_frame() const {
        return demux_state_ ? demux_state_->next_frame : 0;
    }
    // Resolved per-file color spec: matrix/range read from codecpar and
    // reconciled against a decoded-luma probe when a `tv`-style tag lies about
    // full-range data. Every frame this decoder produces (RGBA via the software
    // conversion, NV12 via decode_to_hw) is consistent with this spec.
    [[nodiscard]] gpu::ColorSpec color_spec() const {
        return demux_state_ ? gpu::ColorSpec{demux_state_->matrix, demux_state_->range}
                            : gpu::ColorSpec{gpu::ColorMatrix::BT709, gpu::ColorRange::Limited};
    }
    // Highest valid target frame index for this stream (inclusive), once known.
    // Returns -1 when the encoded extent is not yet known (no frame decoded and
    // neither the container nor stream duration is available).
    [[nodiscard]] int64_t last_frame() const {
        return demux_state_ ? demux_state_->last_frame : -1;
    }

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
    // per-seek container searches. Delegates to the shared DemuxState (and its
    // process-wide cache), so hardware AND software decoders read one index.
    [[nodiscard]] bool has_iframe_index() const {
        return demux_state_ && demux_state_->has_iframe_index();
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
    static const int kPreviewMaxOver = canvas::core::kPreviewMaxOver;

    // Exports a decoded VAAPI hardware frame (as returned by decode_to_hw /
    // decode_to_hw_indexed on a VAAPI-configured decoder) into an owning
    // VaapiSurface: dup'd dmabuf fds + per-plane geometry + a pool pin that
    // keeps the source VA surface reserved while the surface lives. The GL
    // viewer imports the planes via EGLImage for zero-copy NV12 playback.
    // Returns null when the decoder isn't VAAPI-hardware-active, or when the
    // export fails (memory/layout); callers fall back to the CPU NV12/RGBA
    // paths. `frame_number` is recorded on the surface for diagnostics only.
    [[nodiscard]] vaapi::VaapiSurfacePtr vaapi_export_surface(const AVFrame* hw,
                                                              int64_t frame_number) const;

    // Fast-over budget for FULL-RES decodes. Steady playback advances 0-1 frames
    // (sequential), so this only binds far-forward seeks into ultra-sparse GOPs
    // (a 19k-frame keyframe interval caused a 78 s walk that garbled audio by
    // letting it pre-roll ahead). A far full-res seek returns the nearest frame
    // as an approximate still instead of blocking the worker for tens of seconds.
    static const int kFullResMaxOver = canvas::core::kFullResMaxOver;

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
    // Shared demux/decode session (FFmpeg contexts + stream position + the I-frame
    // index). Owned as a unique_ptr so the heap address is stable across moves: the
    // SoftwareDecoder (and the hardware path's call sites) borrow the raw pointer.
    std::unique_ptr<DemuxState> demux_state_;
    // Software decode loop + sws RGBA conversion + SW path counters. demux_ inside
    // is re-pointed at demux_state_ on open() and on adoption (move).
    SoftDecoder sw_decoder_;
    // Negotiated hwaccel pixel format (AV_PIX_FMT_CUDA / AV_PIX_FMT_VAAPI) and the
    // "hardware decode is on" latch. The GPU NV12 fast path and the download-in-
    // convert path both key off them; the tag format AVFrame::format carries for
    // device-memory frames, never the nested sw_format inside hw_frames_ctx.
    int hw_pix_fmt_ = AV_PIX_FMT_NONE;
    bool hw_avail_ = false;
    // Named accelerator this decoder is configured for ("" sw, "vaapi", "cuda",
    // "qsv", "vulkan"): the per-media counterpart of the [hw] probe line, so a
    // multi-GPU box shows WHICH hardware each stream actually decoded on.
    std::string hw_type_name_;
    // Resolved physical GPU name ("" when unknown / software); from the
    // optional `gpu_label` open() argument. Reported in [dec] open.
    std::string hw_gpu_label_;
    // NV12/CUDA engagement tracking: a hardware-configured decoder can still
    // emit SOFTWARE frames when the stream is entered mid-GOP (the AV1 sequence
    // header that primes the NVDEC session lives in an earlier keyframe).
    // hw_engaged_ records the first real CUDA frame; soft_only_ latches when a
    // keyframe re-anchor didn't help so decode_to_hw gives up re-anchoring.
    bool hw_engaged_ = false;
    bool soft_only_ = false;

    // GPU frozen-tail + one-frame lookback. hold_hw_/hold_hw_src_ serve requests
    // past the stream end without re-decoding EOF; retain_hw_/retain_hw_src_ keep
    // a ref-counted copy of the last device frame decode_to_hw served so a
    // repeated source frame (sub-rate transition slot) rides the cheap sequential
    // path instead of a per-repeat GOP re-walk. Both are ref-counted av_frames,
    // independent of the reused demux_state_->av_frame.
    AVFrame* hold_hw_ = nullptr;
    int64_t hold_hw_src_ = -1;
    AVFrame* retain_hw_ = nullptr;
    int64_t retain_hw_src_ = -1;

    // Audio stream decode state, kept in a *separate* AVFormatContext from the
    // video demux (demux_state_->fmt_ctx) so video lookahead and audio decoding
    // each have an independent demux/seek position. Sharing one container made
    // each reader discard the other's packets and issue mutual full-container
    // seeks on every video frame (0.5fps).
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

}  // namespace canvas::core