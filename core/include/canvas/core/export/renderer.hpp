#pragma once

#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/media/audio_decoder.hpp"
#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/media/voice_isolation.hpp"
#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/time_stretch.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/util/render_telemetry.hpp"

#include <cstdint>
#include <functional>
#include <memory>

struct AVBufferRef;
struct AVFrame;

namespace canvas::core {

struct RenderControl {
    std::function<bool()> should_cancel = [] { return false; };
    std::function<void(double progress)> on_progress = [](double) {};
    double fps = 30.0;
};

VideoFramePtr render_video_frame(const Project& project, int64_t tl_frame, int width,
                                 int height,
                                 const AVBufferRef* hw_device_ctx = nullptr);

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

    struct GpuFrameInfo {
        uintptr_t srcY = 0;
        uintptr_t srcUV = 0;
        std::size_t srcYPitch = 0;
        std::size_t srcUVPitch = 0;
        int srcW = 0;
        int srcH = 0;
        const AVFrame* source = nullptr;
        int64_t src_frame = -1;
        int outW = 0;
        int outH = 0;
        int dstW = 0;
        int dstH = 0;
        int dx = 0;
        int dy = 0;
        float fade = 1.0f;
        grade_graph::GradeLutPtr grade;
        int matrix = 1;
        int range = 0;
        const Clip* clip = nullptr;
        bool valid = false;
    };

    bool frame_gpu(int64_t tl_frame, GpuFrameInfo* out);

    void set_telemetry(canvas::core::log::RenderTelemetry* t) { telemetry_ = t; }

    AudioChunkPtr audio_chunk(int64_t tl_sample, int num_frames, int out_sample_rate,
                              int out_channels, double fps);

private:
    struct TrackDecoder {
        const Track* track = nullptr;
        const Clip* active_clip = nullptr;
        int64_t active_tl_in = INT64_MIN;
        int active_media = -1;
        std::unique_ptr<VideoDecoder> dec;
        const Clip* lut_clip = nullptr;
        grade_graph::GradeLutPtr lut;
        grade_graph::GradeLutPtr lut_for(const Clip* clip);
    };

    struct AudioTrackDecoder {
        const Track* track = nullptr;
        const Clip* active_clip = nullptr;
        int64_t active_tl_in = INT64_MIN;
        int active_media = -1;
        std::unique_ptr<AudioDecoder> dec;
    };

    std::vector<TrackDecoder> tracks_;
    std::vector<AudioTrackDecoder> audio_tracks_;
    canvas::core::VoiceIsolationBank iso_bank_;
    canvas::core::EqualizerBank eq_bank_;
    canvas::core::TimeStretchBank stretch_bank_;
    const Project* project_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    const AVBufferRef* hw_device_ctx_ = nullptr;
    canvas::core::log::RenderTelemetry* telemetry_ = nullptr;
};


AudioChunkPtr render_audio_chunk(const Project& project, int64_t tl_sample,
                                 int num_frames, int out_sample_rate, int out_channels,
                                 double fps, RenderControl* control = nullptr);

}