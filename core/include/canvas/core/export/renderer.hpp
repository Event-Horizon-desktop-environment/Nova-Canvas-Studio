#pragma once

#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/timeline/model.hpp"
#include "canvas/core/project/project.hpp"

#include <cstdint>
#include <functional>
#include <memory>

// Forward-declare the (global-namespace) libav buffer/frame structs so that
// parameters below resolve to ::AVBufferRef / ::AVFrame. Writing the elaborated
// `struct AVBufferRef` inside namespace canvas::core would instead inject a distinct
// canvas::core::AVBufferRef type, breaking overload resolution against
// VideoDecoder::open(..., AVBufferRef*).
struct AVBufferRef;
struct AVFrame;

namespace canvas::core {

// Polled cancellation + progress hook. The render loop calls `should_cancel()`
// at least once per output frame and hands the progress (0..1) via `on_progress`
// every frame so the UI can show a live progress bar.
struct RenderControl {
    std::function<bool()> should_cancel = [] { return false; };
    std::function<void(double progress)> on_progress = [](double) {};
    double fps = 30.0;
};

// Renders one RGBA frame of a timeline at timeline frame `tl_frame`, compositing
// the enabled video tracks top-down. Returns nullptr when nothing is visible.
// `width`/`height` are the requested output canvas size; frames are scaled to
// fit the canvas (pillarboxed) and drawn over the lower-track content. Opaque
// hardware decode device is passed through so render can reuse the app's GPU.
VideoFramePtr render_video_frame(const Project& project, int64_t tl_frame, int width,
                                 int height, int max_dim = 0,
                                 const AVBufferRef* hw_device_ctx = nullptr);

// Reusable render session for a full export. Opens one decoder per video track
// up front and reuses it across frames (instead of re-opening the media file
// for every frame, which is the dominant CPU cost during a render). Frame
// compositing semantics are identical to render_video_frame(). Call begin()
// once, then frame(tl_frame) for each output frame. Not thread-safe; keep per
// worker thread.
class RenderSession {
public:
    RenderSession() = default;
    ~RenderSession();
    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

    bool begin(const Project& project, int width, int height,
               const AVBufferRef* hw_device_ctx = nullptr);
    void end();
    VideoFramePtr frame(int64_t tl_frame);

    // Describes a single-clip frame that can be composited directly on the GPU
    // from a hardware-decoded NV12 source, avoiding the full-res CPU canvas
    // blit and the full-res CPU->GPU upload that dominates software exports.
    struct GpuFrameInfo {
        // Source device-plane pointers (from a CUDA hw-decoded NV12 frame).
        uintptr_t srcY = 0;
        uintptr_t srcUV = 0;
        std::size_t srcYPitch = 0;
        std::size_t srcUVPitch = 0;
        int srcW = 0;  // source luma dims (chroma subsampled by 2)
        int srcH = 0;
        // The hardware-decoded source frame itself. BORROWED: the decoder
        // recycles its internal frame on the next decode call, so callers that
        // need it alive across an async kernel must av_frame_ref() it and keep
        // the reference until the kernel has been synchronized.
        const AVFrame* source = nullptr;
        // The source media frame number actually decoded for this output frame
        // (>= the requested src_frame when the forward decoder overshoots).
        int64_t src_frame = -1;
        // Target canvas (== encoder frame size) and letterboxed content rect.
        int outW = 0;
        int outH = 0;
        int dstW = 0;
        int dstH = 0;
        int dx = 0;
        int dy = 0;
        bool valid = false;
    };

    // When the frame at `tl_frame` is produced by exactly one enabled video
    // clip and hardware decode is active, decodes it to the raw GPU NV12 frame
    // (no CPU download, no RGBA conversion, no full-res canvas blit) and fills
    // `out` with the source device planes + letterbox rect. Returns true and
    // sets out->valid when the caller can composite it on the GPU; otherwise
    // returns false and the caller must fall back to frame()/CPU compositing.
    bool frame_gpu(int64_t tl_frame, GpuFrameInfo* out);

    // Decodes + mixes one video-frame-duration of audio at `tl_sample` (in
    // output sample units), reusing persistent per-track AudioDecoders so that
    // sequential calls advance the same decode stream continuously instead of
    // re-opening the file (and re-priming a fresh resampler) every call. This
    // mirrors how frame()/frame_gpu() keep persistent video decoders. Semantics
    // (num_frames x channels interleaved float PCM, mixed from all enabled
    // audio clips) match render_audio_chunk(). Returns empty on no audio.
    AudioChunkPtr audio_chunk(int64_t tl_sample, int num_frames, int out_sample_rate,
                              int out_channels, double fps);

private:
    struct TrackDecoder {
        const Track* track = nullptr;
        const Clip* active_clip = nullptr;
        int64_t active_tl_in = INT64_MIN;  // tl_in of the clip the decoder maps
        int active_media = -1;             // media id the decoder was opened for
        std::unique_ptr<VideoDecoder> dec;
    };

    // Persistent per-audio-track decode state, kept across audio_chunk() calls so
    // the AudioDecoder's internal buffer + resampler stay warm and sequential
    // chunk requests decode continuously instead of re-opening every call.
    struct AudioTrackDecoder {
        const Track* track = nullptr;
        const Clip* active_clip = nullptr;
        int64_t active_tl_in = INT64_MIN;
        int active_media = -1;
        std::unique_ptr<AudioDecoder> dec;
    };

    std::vector<TrackDecoder> tracks_;
    std::vector<AudioTrackDecoder> audio_tracks_;
    const Project* project_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    const AVBufferRef* hw_device_ctx_ = nullptr;
};


// Renders `num_frames` interleaved float PCM frames (in *output* sample units)
// covering the audio of the timeline at `tl_frame` spanning one video frame
// duration (derived from `fps`). Mixed from all enabled/linked audio clips. The
// returned samples are `num_frames * channels` floats. `channels` is fixed to a
// requested layout (default stereo).
AudioChunkPtr render_audio_chunk(const Project& project, int64_t tl_sample,
                                 int num_frames, int out_sample_rate, int out_channels,
                                 double fps, RenderControl* control = nullptr);

}  // namespace canvas::core
