#pragma once

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/media/vaapi/surface.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace canvas::core {

namespace grade_graph {
struct GradeLut3D;
}

struct VideoFrame {
    int64_t pts_ticks = 0;
    double pts_seconds = 0.0;
    int64_t frame_number = 0;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    std::vector<uint8_t> rgba;

    [[nodiscard]] std::size_t bytes() const noexcept { return rgba.size(); }
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

struct Nv12Frame {
    int64_t frame_number = 0;
    int width = 0;
    int height = 0;
    std::size_t y_pitch = 0;
    std::size_t uv_pitch = 0;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;

    gpu::ColorMatrix matrix = gpu::ColorMatrix::BT709;
    gpu::ColorRange range = gpu::ColorRange::Limited;

    vaapi::VaapiSurfacePtr gpu;

    [[nodiscard]] std::size_t bytes() const noexcept { return y.size() + uv.size(); }
    [[nodiscard]] bool has_cpu() const noexcept { return !y.empty() && !uv.empty(); }
    [[nodiscard]] bool has_gpu() const noexcept { return gpu && gpu->valid(); }
};

using Nv12FramePtr = std::shared_ptr<const Nv12Frame>;

enum class TransitionRenderMode {
    None = 0,
    CrossDissolve,
    DipToBlack,
    FadeOut,
    FadeIn,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
};

struct RenderFrame {
    VideoFramePtr a;
    VideoFramePtr b;
    TransitionRenderMode mode = TransitionRenderMode::None;
    float progress = 0.0f;

    bool fade_from_black = false;
    bool fade_to_black = false;

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    double pos_x = 0.0;
    double pos_y = 0.0;
    float rotation_deg = 0.0f;
    double anchor_dx = 0.0;
    double anchor_dy = 0.0;
    bool flip_h = false;
    bool flip_v = false;

    Nv12FramePtr nv12;
    Nv12FramePtr b_nv12;

    std::shared_ptr<const grade_graph::GradeLut3D> grade;
    std::shared_ptr<const grade_graph::GradeLut3D> grade_b;

    [[nodiscard]] bool has_transition() const noexcept {
        return mode != TransitionRenderMode::None && b != nullptr;
    }
};

using RenderFramePtr = std::shared_ptr<const RenderFrame>;

struct AudioChunk {
    int64_t start_sample = 0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;
};

using AudioChunkPtr = std::shared_ptr<const AudioChunk>;

}