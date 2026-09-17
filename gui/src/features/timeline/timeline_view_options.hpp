#pragma once

class QSettings;

namespace canvas::gui {

enum class ThumbnailMode {
    Off,
    SingleFrame,
    Filmstrip,
};

enum class ViewerBackground {
    Black,
    Checkerboard,
    White,
    Gray,
};

struct TimelineViewOptions {
    bool show_stacked_timelines = false;
    bool show_subtitle_tracks = false;
    bool show_waveforms = true;
    bool show_clip_names = true;
    bool show_clip_durations = false;

    ThumbnailMode thumbnails = ThumbnailMode::Filmstrip;
    ViewerBackground viewer_background = ViewerBackground::Black;

    bool non_rectified_waveforms = true;
    bool full_waveforms = true;
    bool waveform_borders = false;
    bool scaled_waveforms = true;

    bool fixed_playhead = false;

    double video_track_height = 60.0;
    double audio_track_height = 60.0;
};

void load_view_options(QSettings& s, TimelineViewOptions& opts);
void save_view_options(QSettings& s, const TimelineViewOptions& opts);

}
