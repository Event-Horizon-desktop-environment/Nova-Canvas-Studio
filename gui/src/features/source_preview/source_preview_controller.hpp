#pragma once

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

    void open_media(const canvas::core::MediaEntry& media, double fallback_fps);
    void close_media();

    void scrub_fraction(double fraction);
    void scrub_to_frame(int64_t frame);

    void play();
    void pause();
    void toggle_play_pause();
    void release_audio();

    void begin_hover_scrub();
    void end_hover_scrub();

    [[nodiscard]] bool has_media() const { return has_media_; }
    [[nodiscard]] bool is_video() const { return has_video_; }
    [[nodiscard]] bool is_audio() const { return has_audio_; }
    [[nodiscard]] bool is_playing() const { return player_.is_playing(); }
    [[nodiscard]] int64_t current_frame() const { return current_frame_; }
    [[nodiscard]] int64_t total_frames() const { return total_frames_; }
    [[nodiscard]] double fps() const { return fps_; }
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

}
