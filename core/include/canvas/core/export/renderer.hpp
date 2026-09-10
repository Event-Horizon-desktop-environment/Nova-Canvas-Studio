#pragma once

#include "canvas/core/grade_graph/lut.hpp"
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

// Polled cancellation + progress hook: the render loop polls `should_cancel()`
// per frame and reports 0..1 progress via `on_progress` for the UI.
struct RenderControl {
    std::function<bool()> should_cancel = [] { return false; };
    std::function<void(double progress)> on_progress = [](double) {};
    double fps = 30.0;
};

// Renders one RGBA frame of a timeline at `tl_frame`, compositing the enabled
// video tracks top-down (pillarboxed). Returns nullptr when nothing is visible.
// An optional hardware decode device lets render reuse the app's GPU.
VideoFramePtr render_video_frame(const Project& project, int64_t tl_frame, int width,
                                 int height, int max_dim = 0,
                                 const AVBufferRef* hw_device_ctx = nullptr);

// Reusable render session for a full export: opens one decoder per video track
// and reuses it across frames (re-opening the media per frame is the dominant
// CPU cost). Semantics match render_video_frame(). Not thread-safe.
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

    // A single-clip frame that can be composited on the GPU from a
    // hardware-decoded NV12 source, avoiding the full-res CPU canvas blit.
    struct GpuFrameInfo {
        // Source device-plane pointers (from a CUDA hw-decoded NV12 frame).
        uintptr_t srcY = 0;
        uintptr_t srcUV = 0;
        std::size_t srcYPitch = 0;
        std::size_t srcUVPitch = 0;
        int srcW = 0;  // source luma dims (chroma subsampled by 2)
        int srcH = 0;
        // The hardware-decoded source frame itself. BORROWED: the decoder
        // recycles it on the next decode call; callers that need it alive across
        // an async kernel must av_frame_ref() it and hold the ref until sync.
        const AVFrame* source = nullptr;
        // Source media frame actually decoded for this output frame (>= the
        // requested src_frame when the forward decoder overshoots).
        int64_t src_frame = -1;
        // Target canvas (== encoder frame size) + letterboxed content rect.
        int outW = 0;
        int outH = 0;
        int dstW = 0;
        int dstH = 0;
        int dx = 0;
        int dy = 0;
        // Whole-canvas edge-fade gain toward black (same law as the CPU
        // compositor's single-clip transition factor; 1.0 = no fade).
        float fade = 1.0f;
        bool valid = false;
    };

    // When exactly one enabled video clip produces `tl_frame` and hw decode is
    // active, decode it to the raw GPU NV12 frame and fill `out` with the source
    // device planes + letterbox rect. Returns true (out->valid) when the caller
    // can composite on the GPU; otherwise fall back to frame()/CPU.
    bool frame_gpu(int64_t tl_frame, GpuFrameInfo* out);

    // Decodes + mixes one video-frame-duration of audio at `tl_sample` using
    // persistent per-track AudioDecoders so sequential calls advance one decode
    // stream continuously (no re-open / resampler re-prime per call). Semantics
    // match render_audio_chunk(). Empty on no audio.
    AudioChunkPtr audio_chunk(int64_t tl_sample, int num_frames, int out_sample_rate,
                              int out_channels, double fps);

private:
    struct TrackDecoder {
        const Track* track = nullptr;
        const Clip* active_clip = nullptr;
        int64_t active_tl_in = INT64_MIN;  // tl_in of the clip the decoder maps
        int active_media = -1;             // media id the decoder was opened for
        std::unique_ptr<VideoDecoder> dec;
        // Baked LUT cache: one entry per active clip, invalidated when the clip
        // pointer changes (each Project snapshot owns fresh Clips), so a graded
        // clip bakes once per session instead of once per frame.
        const Clip* lut_clip = nullptr;
        grade_graph::GradeLutPtr lut;
        grade_graph::GradeLutPtr lut_for(const Clip* clip);
    };

    // Persistent per-audio-track state, kept so sequential audio_chunk() calls
    // decode continuously rather than re-opening and re-priming every call.
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


// Renders `num_frames` interleaved float PCM frames (in output sample units)
// covering one video-frame duration (from `fps`), mixed from all enabled audio
// clips. Returns `num_frames * channels` floats.
AudioChunkPtr render_audio_chunk(const Project& project, int64_t tl_sample,
                                 int num_frames, int out_sample_rate, int out_channels,
                                 double fps, RenderControl* control = nullptr);

}  // namespace canvas::core
