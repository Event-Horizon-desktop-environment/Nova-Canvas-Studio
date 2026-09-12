#include "features/timeline/timeline_view_options.hpp"

#include <QSettings>

namespace canvas::gui {

namespace {

// QSettings key prefix. The pair (org "Nova Canvas", app "canvas") is set in
// main.cpp, so these land under ~/.config/Nova Canvas/canvas.conf.
constexpr const char* kPrefix = "timeline/view/";

template <typename T>
T read(QSettings& s, const char* key, T fallback) {
    const QVariant v = s.value(QString::fromLatin1(kPrefix) + QLatin1String(key), fallback);
    return v.value<T>();
}

template <typename T>
void write(QSettings& s, const char* key, const T& value) {
    s.setValue(QString::fromLatin1(kPrefix) + QLatin1String(key), value);
}

}  // namespace

// Loads the persisted "Set as Default View" snapshot into `opts`. Missing keys
// keep the struct's own defaults, so a partial/default settings file loads as
// the baseline view (matching Resolve's default-scoped behavior).
void load_view_options(QSettings& s, TimelineViewOptions& opts) {
    opts.show_stacked_timelines = read(s, "show_stacked_timelines", opts.show_stacked_timelines);
    opts.show_subtitle_tracks = read(s, "show_subtitle_tracks", opts.show_subtitle_tracks);
    opts.show_waveforms = read(s, "show_waveforms", opts.show_waveforms);
    opts.show_clip_names = read(s, "show_clip_names", opts.show_clip_names);
    opts.show_clip_durations = read(s, "show_clip_durations", opts.show_clip_durations);
    opts.thumbnails = static_cast<ThumbnailMode>(read(s, "thumbnails",
                                                      static_cast<int>(opts.thumbnails)));
    opts.viewer_background = static_cast<ViewerBackground>(
        read(s, "viewer_background", static_cast<int>(opts.viewer_background)));
    opts.non_rectified_waveforms =
        read(s, "non_rectified_waveforms", opts.non_rectified_waveforms);
    opts.full_waveforms = read(s, "full_waveforms", opts.full_waveforms);
    opts.waveform_borders = read(s, "waveform_borders", opts.waveform_borders);
    opts.scaled_waveforms = read(s, "scaled_waveforms", opts.scaled_waveforms);
    opts.fixed_playhead = read(s, "fixed_playhead", opts.fixed_playhead);
    opts.video_track_height = read(s, "video_track_height", opts.video_track_height);
    opts.audio_track_height = read(s, "audio_track_height", opts.audio_track_height);
}

void save_view_options(QSettings& s, const TimelineViewOptions& opts) {
    write(s, "show_stacked_timelines", opts.show_stacked_timelines);
    write(s, "show_subtitle_tracks", opts.show_subtitle_tracks);
    write(s, "show_waveforms", opts.show_waveforms);
    write(s, "show_clip_names", opts.show_clip_names);
    write(s, "show_clip_durations", opts.show_clip_durations);
    write(s, "thumbnails", static_cast<int>(opts.thumbnails));
    write(s, "viewer_background", static_cast<int>(opts.viewer_background));
    write(s, "non_rectified_waveforms", opts.non_rectified_waveforms);
    write(s, "full_waveforms", opts.full_waveforms);
    write(s, "waveform_borders", opts.waveform_borders);
    write(s, "scaled_waveforms", opts.scaled_waveforms);
    write(s, "fixed_playhead", opts.fixed_playhead);
    write(s, "video_track_height", opts.video_track_height);
    write(s, "audio_track_height", opts.audio_track_height);
}

}  // namespace canvas::gui