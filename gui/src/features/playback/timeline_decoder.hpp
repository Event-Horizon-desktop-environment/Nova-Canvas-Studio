#pragma once

// TimelineDecoder — the Qt-free video decode front-end for playback and scrub.
// Extracted from SequenceController so the decode path itself has no Qt
// dependency and can run in headless unit tests (links only canvas_core).
//
// WHAT IT OWNS
//   * One DecoderSlot (VideoDecoder + byte-budgeted FrameCache) per media id;
//     add_media() opens the decoder (hardware when available), close() drops all.
//   * The low-res scrub-preview LRU (PreviewKey/preview_cache_/preview_lru_),
//     separate from the full-res caches so a preview never displaces (or is
//     returned as) a crisp playback frame.
//   * The shared hardware-decode device (HwDeviceManager), probed once and
//     reused by every slot + the GPU NV12 composite path.
//
// INVARIANT: headless — may include only <system>, <canvas/core/...> and other
// headless modules, never <Q...>.
//
// FROZEN API: this public surface is the stable playback seam. Changes to
// existing signatures require the refactor plan's sign-off; additive methods
// are fine.

#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/frame_cache.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"
#include "canvas/core/project/project.hpp"

namespace canvas::gui {

class TimelineDecoder {
public:
    // Create/load the decoder slot for `entry` (VideoDecoder + FrameCache).
    void add_media(const canvas::core::MediaEntry& entry);
    // Drop every decoder slot (project swap). The preview LRU and the shared
    // hardware device are also reset.
    void close();
    // Drop the slot + preview entries for a single media id, forcing the next
    // decode() to reopen/re-decode from scratch (e.g. after an offline switch).
    void invalidate(canvas::core::MediaId media);

    // Decodes the media frame for `clip` at timeline position `seq_frame`.
    // max_dim == 0 → full-res frame through the slot's FrameCache; max_dim > 0 →
    // reduced preview frame through the low-res LRU (never poisons the cache).
    canvas::core::VideoFramePtr decode(const canvas::core::Project& project,
                                   const canvas::core::Clip& clip,
                                   std::int64_t seq_frame, int max_dim = 0);
    // GPU fast path: hardware decode straight to the device (NVDEC), composite
    // (resize) on the GPU, download only the small NV12 planes for the viewer's
    // YUV shader. Engages only when CUDA is available AND this slot is hardware-
    // decoding; returns nullptr (caller falls back to decode / RGBA) on any
    // failure, including backward scrubs (decode_to_hw only decodes forward).
    canvas::core::Nv12FramePtr decode_nv12(const canvas::core::Project& project,
                                       const canvas::core::Clip& clip,
                                       std::int64_t seq_frame, int max_dim);
    // Black fallback frame sized to the clip's media (or 1920x1080 if the slot
    // is not yet loaded), capped at max_dim like every other decode.
    canvas::core::VideoFramePtr make_black_frame(const canvas::core::Clip& clip,
                                             int max_dim = 0) const;

    // Full-res timeline assembly: builds the RenderFrame for `seq_frame` (top
    // clip + transition a/b handling + single-clip fades).
    canvas::core::RenderFramePtr frame(const canvas::core::Project& project,
                                   std::int64_t seq_frame);
    // Reduced-resolution variant used only for scrubbing; never writes the
    // reduced frame into the full-res cache.
    canvas::core::RenderFramePtr preview(const canvas::core::Project& project,
                                     std::int64_t seq_frame, int max_dim);
    // Preview-LRU hit/miss accounting since the last call (additive, FROZEN-safe).
    // Scrub drags consume it at [scrub] END to report the per-drag cache ratio.
    struct PreviewStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
    };
    [[nodiscard]] PreviewStats take_preview_stats();
    // Window accrual of the per-frame grade-apply cost (Phase 6 live graded
    // preview). The decoder's `[grade]` lines own the throttled per-frame
    // reporting (engage census, per-30 summary, apply-spike warning); this is
    // the cross-frame totals the controller folds into its per-second `[play]`
    // health snapshot. Resets on every read.
    struct GradeStats {
        std::uint64_t samples = 0;
        double ms_sum = 0.0;
        double ms_max = 0.0;
    };
    [[nodiscard]] GradeStats take_grade_stats();
    // Media frame rate at `seq_frame` (what frame interval the playhead advances
    // at), or `fallback_fps` when nothing covers it.
    double media_rate_at(const canvas::core::Project& project, std::int64_t seq_frame,
                         double fallback_fps) const;

    // Shared hardware-decode device (probed once; falls back to software).
    [[nodiscard]] const canvas::core::HwDeviceManager& hw() const noexcept { return hw_; }

    // Slot diagnostics for the [scrub:BAD] trace (media slot loaded / hw-decoding).
    [[nodiscard]] bool is_loaded(canvas::core::MediaId id) const;
    [[nodiscard]] bool is_hardware(canvas::core::MediaId id) const;

private:
    struct DecoderSlot {
        canvas::core::VideoDecoder decoder;
        canvas::core::FrameCache cache;
        bool loaded = false;
    };

    // (media, source frame) → reduced preview frame. LRU-bounded (32) so repeated
    // scrubbing across the same frames is instant.
    struct PreviewKey {
        canvas::core::MediaId media = 0;
        std::int64_t frame = 0;
        bool operator==(const PreviewKey& o) const noexcept {
            return media == o.media && frame == o.frame;
        }
    };
    struct PreviewKeyHash {
        std::size_t operator()(const PreviewKey& k) const noexcept {
            std::size_t h = static_cast<std::size_t>(k.media) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.frame) * 0x9E3779B97F4A7C15ULL;
            return h;
        }
    };

    std::unordered_map<canvas::core::MediaId, std::unique_ptr<DecoderSlot>> slots_;
    std::unordered_map<PreviewKey, canvas::core::VideoFramePtr, PreviewKeyHash> preview_cache_;
    std::deque<PreviewKey> preview_lru_;
    static constexpr std::size_t kPreviewCacheMax = 32;
    std::uint64_t preview_hits_ = 0;
    std::uint64_t preview_misses_ = 0;
    std::uint64_t preview_evictions_ = 0;

    canvas::core::HwDeviceManager hw_;

    // Topmost unlocked video clip covering `seq_frame` (bottom-to-top scan).
    const canvas::core::Clip* top_video_clip_at(const canvas::core::Project& project,
                                            std::int64_t seq_frame) const;

    // Phase 6 live graded preview: applies `clip`'s grade to a decoded RGBA
    // frame (returns the raw frame when the clip has no grade or the evaluator
    // has no terminal to evaluate). Emits the always-on `[grade]` telemetry and
    // accrues apply-time into the take_grade_stats() window. Private member so
    // the stats land on the decoder (not file-local); the frozen public surface
    // is untouched by this addition.
    canvas::core::VideoFramePtr grade_clip_frame(const canvas::core::Clip& clip,
                                                 canvas::core::VideoFramePtr frame);

    // Resolve-style grade flattening: returns the baked 3D LUT for `clip`'s
    // grade tree (nullptr when the clip has no wired grade => passthrough).
    // The LUT is cached per clip and re-baked only on a grade change; the
    // viewer attaches it to the NV12 fast path so the grade applies on the GPU,
    // while the CPU RGBA path applies the SAME LUT — preview == export by
    // construction. Also logs the always-on `[grade]` engage census on bake.
    canvas::core::grade_graph::GradeLutPtr grade_lut_for(const canvas::core::Clip& clip);

    std::uint64_t grade_samples_ = 0;
    double grade_ms_sum_ = 0.0;
    double grade_ms_max_ = 0.0;
    // Baked LUT cache: clip -> LUT, so scrubbing through a graded timeline
    // never re-runs the (already cheap) grid bake on every frame. Keyed by clip
    // pointer: Project snapshots own fresh Clip objects (pointer changes on any
    // edit), while a clip's pointer is stable within one snapshot — keying on
    // pointer is therefore precise and invalidates on every real grade change.
    std::unordered_map<const canvas::core::Clip*,
                       canvas::core::grade_graph::GradeLutPtr>
        grade_lut_cache_;
};

}  // namespace canvas::gui