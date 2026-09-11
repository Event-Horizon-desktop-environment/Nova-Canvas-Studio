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
//   * The off-thread transition pre-render: a dedicated background thread that,
//     once the playhead is within a short lead of a same-media cross-dissolve
//     with a DISTANT source position, decodes the whole window (A tail + B
//     pre-roll) into in-memory Nv12 planes and parks a fresh decoder at B's
//     head. frame() then serves the baked planes inside the window (zero decode
//     on the playback thread) and adopts the parked decoder as the main slot
//     once past it, so neither the dissolve nor the post-cut boundary ever
//     pays the multi-second far keyframe walk on the worker.
//
// INVARIANT: headless — may include only <system>, <canvas/core/...> and other
// headless modules, never <Q...>.
//
// FROZEN API: this public surface is the stable playback seam. Changes to
// existing signatures require the refactor plan's sign-off; additive methods
// are fine.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

    // CPU-graded CPU preview feed for the Color-page scopes. On the GPU fast
    // path a graded clip needs no CPU pixels to DISPLAY (the grade rides as a
    // GPU-sampled 3D LUT on the NV12 planes), so the extra 640x360 RGBA decode
    // + CPU LUT apply per frame is a pure cost — it exists only so the Color
    // page's scopes/curve-veil can read graded pixels. Off by default: Edit-tab
    // and media playback stay on the GPU fast path. The Color page enables it
    // on enter (and re-presents the current frame) so its scopes show signal.
    void set_cpu_graded_preview_enabled(bool on) { cpu_graded_preview_enabled_ = on; }
    [[nodiscard]] bool cpu_graded_preview_enabled() const noexcept { return cpu_graded_preview_enabled_; }

    // Slot diagnostics for the [scrub:BAD] trace (media slot loaded / hw-decoding).
    [[nodiscard]] bool is_loaded(canvas::core::MediaId id) const;
    [[nodiscard]] bool is_hardware(canvas::core::MediaId id) const;

    // --- Transition pre-render discovery (pure; no I/O, no thread) ---
    // The next same-media cross-dissolve window at-or-ahead of `seq_frame` that
    // the off-thread pre-render would bake: [win_start,win_end) timeline frames.
    // Returns {} when nothing within the lookahead qualifies: no transition, a
    // window longer than the bake cap, a single-clip fade, distinct media, a
    // higher track covering the window head, or no incoming clip at the cut.
    struct TransitionBakeCandidate {
        std::int64_t win_start = -1;
        std::int64_t win_end = -1;
    };
    [[nodiscard]] std::optional<TransitionBakeCandidate>
    next_transition_bake_candidate(const canvas::core::Project& project,
                                   std::int64_t seq_frame) const;

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
    std::unordered_map<canvas::core::MediaId, std::unique_ptr<DecoderSlot>> b_slots_;
    std::unordered_map<PreviewKey, canvas::core::VideoFramePtr, PreviewKeyHash> preview_cache_;
    std::deque<PreviewKey> preview_lru_;
    static constexpr std::size_t kPreviewCacheMax = 32;
    std::uint64_t preview_hits_ = 0;
    std::uint64_t preview_misses_ = 0;
    std::uint64_t preview_evictions_ = 0;

    canvas::core::HwDeviceManager hw_;

    // Off by default → Edit-tab/media playback stay pure GPU (NV12 fast path +
    // GPU LUT); set only while the Color page is active so its scopes/veil can
    // read the CPU-graded preview feed.
    bool cpu_graded_preview_enabled_ = false;

    // Topmost unlocked video clip covering `seq_frame` (bottom-to-top scan).
    const canvas::core::Clip* top_video_clip_at(const canvas::core::Project& project,
                                            std::int64_t seq_frame) const;

    // Shared body of the two NV12 decodes below: builds the I-frame index,
    // verifies hardware+CUDA, maps seq_frame → src_frame, decodes + resizes.
    // `slot` must already be loaded.
    canvas::core::Nv12FramePtr decode_nv12_slot(DecoderSlot* slot,
                                                const canvas::core::Project& project,
                                                const canvas::core::Clip& clip,
                                                std::int64_t seq_frame, int max_dim);

    // Transition-B decode: a same-media cross-dissolve needs TWO decode
    // positions from ONE media file (A's tail + B's pre-roll handle), but the
    // per-media slot `slots_` can hold only one. Decoding B on A's slot made
    // every dissolve frame re-walk TWO GOPs back and forth (~70-100ms each →
    // ~10fps in a 120-frame 2K60 dissolve, measured as a 4.4s A/V drift). These
    // per-clip-id slots give B its own forward-sequential decoder so A and B
    // both walk forward together. Lazy-opened from the clip's media on first
    // use; pruned with the rest on close()/invalidate().
    void open_b_slot(const canvas::core::Project& project,
                     const canvas::core::Clip& clip);

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
    // Baked LUT cache: (clip id, grade change_seq) -> LUT, so scrubbing through
    // a graded timeline never re-runs the (already cheap) grid bake on every
    // frame. Keyed by clip id + the grade's change_seq (bumped on every edit,
    // and carried into the LUT at bake) because the clip POINTER is not a
    // stable identity across snapshots: a grade swap installs a fresh
    // deep-copied snapshot whose clip can land on the SAME address as the freed
    // previous one, making a pointer-keyed lookup return the OLD LUT for the
    // current grade (the preview flicker between current and stale grades). The
    // (id, seq) key is exact, invalidates on every real grade change, and is
    // the same pairing the [grade] bake/upload logs already use to correlate.
    struct GradeLutKey {
        canvas::core::ClipId clip_id = 0;
        std::uint64_t change_seq = 0;
        bool operator==(const GradeLutKey&) const = default;
    };
    struct GradeLutKeyHash {
        std::size_t operator()(const GradeLutKey& k) const noexcept {
            return std::hash<std::uint64_t>()(
                       static_cast<std::uint64_t>(k.clip_id) ^
                       (k.change_seq * 0x9E3779B97F4A7C15ull)) ^
                   std::hash<std::uint64_t>()(k.change_seq);
        }
    };
    std::unordered_map<GradeLutKey, canvas::core::grade_graph::GradeLutPtr,
                       GradeLutKeyHash>
        grade_lut_cache_;

    // ---- Off-thread transition pre-render (in-memory bake) ----
    //
    // A same-media cross-dissolve whose incoming clip starts at a DISTANT
    // source position forces two far keyframe walks on the playback thread: the
    // B-slot open walking B's pre-roll, then (once the window ends) the main
    // slot jumping from A's tail to B's head — each ~1.9s on sparse-GOP footage,
    // freezing the dissolve and drifting audio past the cut. Instead, when the
    // playhead is still ahead of the window (kTransitionBakeLead), a dedicated
    // background thread opens its OWN A/B decoder sessions, walks the whole
    // window into in-memory Nv12 planes (A tail + B pre-roll), and parks a fresh
    // B decoder at B's head. Inside the window frame() serves the cached planes
    // to the viewer's crossfade shader (identical mode/progress/grades to the
    // live path, so zero decode on the worker); once past the window the parked
    // B decoder is adopted into slots_ so the post-cut boundary continues
    // sequentially. Transitions outside the caps — longer than
    // kTransitionBakeMaxFrames, distinct media, single-clip fades, no incoming
    // clip — and scrubs keep the existing live/b-slot path untouched.
    //
    // Everything below is guarded by bake_mutex_ (bake_cv_ wakes the thread;
    // bake_stop_ aborts an in-flight bake so close() can join promptly). The
    // thread is started lazily on the first kick and joins on close().

    struct TransitionBakeJob {
        canvas::core::MediaEntry a_entry;
        canvas::core::MediaEntry b_entry;  // same path as a_entry for a same-media cut
        double seq_fps = 0.0;
        canvas::core::Clip a;
        canvas::core::Clip b;
        std::int64_t win_start = 0;
        std::int64_t win_end = 0;  // exclusive
    };
    struct BakedTransition {
        canvas::core::ClipId a_id = 0;
        canvas::core::MediaId b_media = -1;
        std::int64_t win_start = 0;
        std::int64_t win_end = 0;  // exclusive
        // Per timeline frame (index = seq - win_start): the A tail plane and the
        // B pre-roll plane the viewer's NV12 shader crossfades.
        std::vector<std::pair<canvas::core::Nv12FramePtr, canvas::core::Nv12FramePtr>> planes;
        // The bake's B session, parked at B's head; adopted into slots_ once the
        // playhead passes win_end (VideoDecoder is movable so ownership transfers).
        std::unique_ptr<canvas::core::VideoDecoder> parked_b;
        // Frames served out of this baked window by frame() (observability: the
        // serve path is otherwise silent in the log, so cache engagement was
        // previously impossible to verify).
        std::int64_t served = 0;
    };

    // Fills `out` (window + media/clip copies) for the nearest qualifying same-media
    // transition ahead of `seq_frame`, or returns false.
    bool transition_bake_candidate(const canvas::core::Project& project,
                                   std::int64_t seq_frame,
                                   TransitionBakeJob* out) const;
    // Kick the bake thread when a candidate is current and none is in-flight.
    void maybe_start_transition_bake(const canvas::core::Project& project,
                                     std::int64_t seq_frame);
    // Once the playhead passes a baked window: adopt the parked-B decoder into
    // slots_ (post-cut boundary becomes a sequential continue) and drop the
    // result. The worker thread calls this; `parked_b` transfer is safe because
    // the bake thread owns it no longer once the result is stored.
    void adopt_or_clear_transition_bake(std::int64_t seq_frame);
    void transition_bake_thread();
    void run_transition_bake(const TransitionBakeJob& job);

    // Lead in timeline frames: a bake starts once the playhead is this close to a
    // bake-eligible window (~3.2s at 30fps; the now-sequential bake of a 16-frame
    // window takes ~0.3s, so even a seek-triggered kick at half the lead lands with
    // a generous margin before the window starts).
    static constexpr std::int64_t kTransitionBakeLead = 96;
    // Longest window (timeline frames) the in-memory bake covers; longer windows
    // keep the live path. Bound keeps the held-plane memory small.
    static constexpr std::int64_t kTransitionBakeMaxFrames = 16;

    std::mutex bake_mutex_;
    std::condition_variable bake_cv_;
    std::thread bake_thread_;
    std::unique_ptr<TransitionBakeJob> bake_job_;
    bool bake_inflight_ = false;
    bool bake_stop_ = false;
    std::unique_ptr<BakedTransition> bake_result_;
};

}  // namespace canvas::gui