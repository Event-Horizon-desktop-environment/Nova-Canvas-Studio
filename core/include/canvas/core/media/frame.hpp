#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace canvas::core {

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

// CPU-readable NV12 planes. Produced by the GPU composite path (hardware
// decode -> nv12Resize on the device -> download) so the viewer can upload the
// small Y/UV planes as textures and convert to RGB in a shader, instead of
// paying for a full-res CPU RGBA round-trip during playback/scrub.
struct Nv12Frame {
    int64_t frame_number = 0;
    int width = 0;       // luma dims (chroma is subsampled by 2)
    int height = 0;
    std::size_t y_pitch = 0;   // bytes per luma row
    std::size_t uv_pitch = 0;  // bytes per chroma row (interleaved CbCr)
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;

    [[nodiscard]] std::size_t bytes() const noexcept { return y.size() + uv.size(); }
};

using Nv12FramePtr = std::shared_ptr<const Nv12Frame>;

// How the viewer should composite `b` (incoming) over `a` (outgoing) during a
// transition frame. Mirrors canvas::core::TransitionType for the renderable ones.
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

// A single frame handed to the GPU viewer. Usually only `a` is set (no
// transition); during a transition window `b` and `progress`/`mode` are filled
// so the viewer can blend the two textures in a shader.
struct RenderFrame {
    VideoFramePtr a;                 // outgoing (or only) frame
    VideoFramePtr b;                 // incoming frame during a transition
    TransitionRenderMode mode = TransitionRenderMode::None;
    float progress = 0.0f;           // 0..1 normalized transition progress

    // Single-clip edge fades that do NOT need a second frame. These apply to a
    // clip's own leading (IN) or trailing (OUT) edge when there is no adjacent
    // clip to crossfade against: `fade_from_black` ramps `a` in from black over
    // `progress` (0..1), `fade_to_black` ramps `a` out to black. `b` is empty
    // for these; `mode` is set to one of the Fade modes. The viewer renders
    // them by alpha-blending `a` against black, independent of `has_transition()`
    // (which still requires a real second frame `b` for two-clip crossfades).
    bool fade_from_black = false;
    bool fade_to_black = false;

    // Preview transform for the top clip (viewport only). Mirrors the clip's
    // visual transform in output-pixel units; all identity defaults mean an
    // untransformed clip displays exactly as before. The viewer applies scale /
    // pixel offset / rotation about the anchor / flips to its letterbox quad on
    // the CPU. Compositing (opacity/blend mode) is deliberately NOT previewed —
    // multilayer blending is an export-renderer feature.
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    double pos_x = 0.0;
    double pos_y = 0.0;
    float rotation_deg = 0.0f;
    double anchor_dx = 0.0;
    double anchor_dy = 0.0;
    bool flip_h = false;
    bool flip_v = false;

    // GPU fast path: NV12 planes produced by the hardware-decode + GPU
    // composite path. When set, the viewer should prefer these (upload Y/UV as
    // textures, convert to RGB in the shader) over downloading `a` as RGBA.
    // Only set for the single-clip, no-transition case (like the exporter's
    // GPU fast path); transitions still use the RGBA `a`/`b` frames.
    Nv12FramePtr nv12;

    [[nodiscard]] bool has_transition() const noexcept {
        return mode != TransitionRenderMode::None && b != nullptr;
    }
};

using RenderFramePtr = std::shared_ptr<const RenderFrame>;

// Interleaved float PCM audio in the [-1.0, 1.0] range, used for playback.
struct AudioChunk {
    int64_t start_sample = 0;  // sample index within the source media
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;
};

using AudioChunkPtr = std::shared_ptr<const AudioChunk>;

}
