#pragma once

// Dual-Viewer SourcePreviewController (splitplan "own folder + own cpp/hpp"
// rule). Qt module: wraps a dedicated SequenceController (its own worker
// thread, TimelineDecoder slots, and AudioPipeline) so a media-pool entry can
// be previewed exactly like a timeline clip — GPU scrub previews via
// seek_preview() and real A/V playback via play() — without the main timeline
// controller ever being disturbed.
//
// Audio ownership is exclusive: only one controller may hold the output
// device. Starting this player calls release_audio() on any previously held
// device; the MainWindow cross-pause hook calls release_audio() the opposite
// way when the timeline plays. Everything but the QObject wrapper is Qt-owned
// by SequenceController itself (a Qt object), so this module is the single
// place that knows "the source preview player".

#include <QObject>

#include <cstdint>
#include <memory>
#include <string>

#include "canvas/core/media/frame.hpp"
#include "canvas/core/project/project.hpp"
#include "features/playback/sequence_controller.hpp"

namespace canvas::gui::source_preview {

class SourcePreviewController final : public QObject {
    Q_OBJECT

public:
    explicit SourcePreviewController(QObject* parent = nullptr);

    // Opens a media-pool entry as a synthetic single-clip preview. Refreshes
    // the player only when the media actually changed (hover re-skims the same
    // tile without tearing down the projector). Releases the output device
    // first so a playing source never competes with the timeline.
    void open_media(const canvas::core::MediaEntry& media, double fallback_fps);
    void close_media();

    // Hover/panel scrub: preview-decode the frame at a fraction / frame of the
    // source. Uses the fast low-res preview path; a committed position should
    // follow via play()/seek() semantics (the synthetic project handles it).
    void scrub_fraction(double fraction);
    void scrub_to_frame(int64_t frame);

    void play();
    void pause();
    void toggle_play_pause();
    // Cross-player audio handoff: pauses and closes the output device so the
    // OTHER controller can open it (two AudioOutputs can't share the device).
    void release_audio();

    // Audible hover-scrub session lifecycle (media-pool scrub). The pool drives
    // seek_preview directly, which only opens the device for audible grains
    // when it transitions out of a fresh scrub state — so the shell must open
    // that session on hover start and close it on hover end, exactly like the
    // timeline's begin_scrub/end_scrub pair around a drag. end_hover_scrub
    // CLOSES the output device, handing audio back to the timeline.
    void begin_hover_scrub();
    void end_hover_scrub();

    [[nodiscard]] bool has_media() const { return has_media_; }
    [[nodiscard]] bool is_video() const { return has_video_; }
    [[nodiscard]] bool is_audio() const { return has_audio_; }
    [[nodiscard]] bool is_playing() const { return player_.is_playing(); }
    [[nodiscard]] int64_t current_frame() const { return current_frame_; }
    [[nodiscard]] int64_t total_frames() const { return total_frames_; }
    [[nodiscard]] double fps() const { return fps_; }
    // Absolute media path of the currently open entry (empty when closed); the
    // shell derives the display name the same way the pool does.
    [[nodiscard]] const std::string& media_path() const { return media_path_; }

signals:
    void frame_ready(canvas::core::RenderFramePtr frame);
    void position_changed(int64_t frame_number);
    void playback_changed(bool playing);
    void media_changed(bool has_media);

private:
    void on_position_changed(int64_t frame);

    SequenceController player_;
    std::shared_ptr<const canvas::core::Project> project_;
    std::string media_path_;
    bool has_media_ = false;
    bool has_video_ = false;
    bool has_audio_ = false;
    double fps_ = 0.0;
    int64_t total_frames_ = 0;
    int64_t current_frame_ = 0;
};

}  // namespace canvas::gui::source_preview