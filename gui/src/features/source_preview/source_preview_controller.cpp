#include "features/source_preview/source_preview_controller.hpp"

#include <QDebug>

#include <cstdint>

#include "Logging.hpp"
#include "features/source_preview/source_preview_model.hpp"

namespace canvas::gui::source_preview {

SourcePreviewController::SourcePreviewController(QObject* parent) : QObject(parent) {
    connect(&player_, &SequenceController::frame_ready, this,
            &SourcePreviewController::frame_ready);
    connect(&player_, &SequenceController::position_changed, this,
            &SourcePreviewController::on_position_changed);
    connect(&player_, &SequenceController::playback_changed, this,
            &SourcePreviewController::playback_changed);
}

void SourcePreviewController::open_media(const canvas::core::MediaEntry& media,
                                         const double fallback_fps) {
    if (has_media_ && media_path_ == media.path) {
        if (debug_enabled())
            qDebug() << "[srcprv] open_media same-entry re-skim path="
                     << QString::fromStdString(media.path);
        return;
    }
    player_.end_scrub();
    release_audio();

    has_media_ = true;
    has_video_ = media.width > 0 && media.height > 0;
    has_audio_ = media.has_audio;
    fps_ = media.fps > 0.0 ? media.fps : (fallback_fps > 0.0 ? fallback_fps : 30.0);
    total_frames_ = media.total_frames > 0 ? media.total_frames : 1;
    current_frame_ = 0;
    media_path_ = media.path;

    qWarning().nospace()
        << "[srcprv] open_media path=" << QString::fromStdString(media.path)
        << " video=" << has_video_
        << " audio=" << has_audio_
        << " fps=" << fps_
        << " total_frames=" << total_frames_
        << " fallback_fps=" << fallback_fps;

    project_ = build_source_project(media, fallback_fps);
    player_.set_project(project_, 0);
    emit media_changed(true);
}

void SourcePreviewController::close_media() {
    if (!has_media_ && !project_) return;
    release_audio();
    qWarning() << "[srcprv] close_media"
               << " path=" << QString::fromStdString(media_path_);
    has_media_ = false;
    has_video_ = false;
    has_audio_ = false;
    fps_ = 0.0;
    total_frames_ = 0;
    current_frame_ = 0;
    media_path_.clear();
    project_.reset();
    player_.set_project(nullptr, 0);
    emit media_changed(false);
}

void SourcePreviewController::scrub_fraction(const double fraction) {
    if (!has_media_) {
        if (debug_enabled())
            qDebug() << "[srcprv] scrub_fraction ignored (no media) frac=" << fraction;
        return;
    }
    if (debug_enabled())
        qDebug().nospace() << "[srcprv] scrub_fraction frac=" << fraction
                           << " frame=" << fraction_to_source_frame(fraction, total_frames_);
    player_.seek_preview(fraction_to_source_frame(fraction, total_frames_));
}

void SourcePreviewController::scrub_to_frame(const int64_t frame) {
    if (!has_media_) return;
    if (debug_enabled())
        qDebug() << "[srcprv] scrub_to_frame frame=" << frame;
    player_.seek_preview(frame);
}

void SourcePreviewController::play() {
    qWarning() << "[srcprv] play";
    player_.play();
}
void SourcePreviewController::pause() {
    qWarning() << "[srcprv] pause";
    player_.pause();
}
void SourcePreviewController::toggle_play_pause() { player_.toggle_play_pause(); }

void SourcePreviewController::release_audio() { player_.release_audio(); }

void SourcePreviewController::begin_hover_scrub() {
    if (debug_enabled()) qDebug() << "[srcprv] begin_hover_scrub";
    player_.begin_scrub();
}
void SourcePreviewController::end_hover_scrub() {
    if (debug_enabled()) qDebug() << "[srcprv] end_hover_scrub";
    player_.end_scrub();
}

void SourcePreviewController::on_position_changed(const int64_t frame) {
    current_frame_ = frame;
    emit position_changed(frame);
}

}
