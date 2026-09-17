#pragma once

#include <QWidget>

#include <cstdint>

class QImage;
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

    void set_media_info(const QString& name, bool is_video, bool is_audio, int64_t total_frames);
    void set_media_position(int64_t frame, double fps);
    void set_playing(bool playing);
    void clear_media();
    void set_audio_waveform(const QImage& image);

signals:
    void play_clicked();
    void scrub_fraction(double fraction);

private:
    void update_time_label(int64_t frame, double fps);

    class AudioSpectrumView;
    AudioSpectrumView* audio_spectrum_ = nullptr;
    ViewerGL* viewer_ = nullptr;
    QStackedLayout* stacked_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* time_label_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QSlider* mini_scrub_ = nullptr;
    QWidget* empty_state_ = nullptr;
    int64_t total_frames_ = 0;
    bool slider_down_ = false;
    bool audio_mode_ = false;
};

}
