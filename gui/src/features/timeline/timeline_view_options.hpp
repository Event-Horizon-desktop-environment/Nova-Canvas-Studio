#pragma once

// Timeline view-options model (DaVinci Resolve's Timeline > View Options
// dropdown). Pure data with no Qt, so the timeline renderer and the transport
// bar's menu builder can share one struct without coupling. One instance is
// owned by MainWindow; widgets hold a borrowed pointer to it. Like Resolve,
// menu edits are session-scoped by default — only "Set as Default View"
// persists the current combination into QSettings (loaded again at startup).
//
// Two flags (show_stacked_timelines, show_subtitle_tracks) are wired to the
// menu but have no backing feature yet: the engine has a single sequence and
// no subtitle lanes, so they persist and toggle inert.

class QSettings;  // QtCore forward decl — the struct below is Qt-free data

namespace canvas::gui {

// How video clip thumbnails render inside the clip body.
enum class ThumbnailMode {
    Off,          // no strip at all — the shell renders as a flat block
    SingleFrame,  // the single source frame stretched across the clip
    Filmstrip,    // continuous multi-cell strip, resampled as you zoom
};

// What fills the viewer canvas behind the (letterboxed) frame image.
enum class ViewerBackground {
    Black,
    Checkerboard,
    White,
    Gray,
};

struct TimelineViewOptions {
    // Top toggles.
    bool show_stacked_timelines = false;
    bool show_subtitle_tracks = false;
    bool show_waveforms = true;
    bool show_clip_names = true;
    bool show_clip_durations = false;

    // Submenu selections (radio groups).
    ThumbnailMode thumbnails = ThumbnailMode::Filmstrip;
    ViewerBackground viewer_background = ViewerBackground::Black;

    // Waveform-detail toggles (only painted while show_waveforms is on).
    bool non_rectified_waveforms = true;  // bipolar (mirrored); off = single-sided
    bool full_waveforms = true;           // full body height; off = ~62% centered
    bool waveform_borders = false;        // hairline box around the wave body
    bool scaled_waveforms = true;         // off = ~60% of the computed height

    // Fixed playhead: keep the playhead visually anchored (the timeline scolls
    // underneath it) rather than a needle traveling across a static track.
    bool fixed_playhead = false;

    // Global Track Height sliders (overrides every row of that kind).
    double video_track_height = 60.0;
    double audio_track_height = 60.0;
};

// Persist options under the "timeline/view/" key group, one per field.
// load_view_options applies only the keys that are present (per-key partial
// defaults); save_view_options writes the whole combination. Called by the
// view-options menu's "Set as Default View" and by startup wiring.
void load_view_options(QSettings& s, TimelineViewOptions& opts);
void save_view_options(QSettings& s, const TimelineViewOptions& opts);

}  // namespace canvas::gui