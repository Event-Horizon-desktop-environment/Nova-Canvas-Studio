#pragma once

// Dual-Viewer Source pane (splitplan "own folder + own cpp/hpp" rule). Qt
// widget chrome only: a slim transport header (clip name, position timecode,
// play/pause, mini-scrub) above a Source-mode ViewerGL. All playback + decode
// semantics live in SourcePreviewController; this panel just reflects it and
// re-broadcasts user intent (play_clicked / scrub_fraction) so the shell can
// wire it without reaching into the controller.

#include <QWidget>

#include <cstdint>

class QLabel;
class QSlider;
class QStackedLayout;
class QToolButton;

namespace canvas::gui {
class ViewerGL;
}

namespace canvas::gui::source_preview {

class SourceViewerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SourceViewerPanel(QWidget* parent = nullptr);

    ViewerGL* viewer() const { return viewer_; }

    // Reflect an opened/closed media entry (frame/time band + empty state).
    void set_media_info(const QString& name, bool is_video, bool is_audio, int64_t total_frames);
    void set_media_position(int64_t frame, double fps);
    void set_playing(bool playing);
    void clear_media();

signals:
    void play_clicked();
    // Panel mini-scrub drag / click: fraction of the source (0..1).
    void scrub_fraction(double fraction);

private:
    void update_time_label(int64_t frame, double fps);

    ViewerGL* viewer_ = nullptr;
    QStackedLayout* stacked_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* time_label_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QSlider* mini_scrub_ = nullptr;
    QWidget* empty_state_ = nullptr;
    int64_t total_frames_ = 0;
    bool slider_down_ = false;
};

}  // namespace canvas::gui::source_preview