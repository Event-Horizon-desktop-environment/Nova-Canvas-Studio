#include "features/source_preview/source_viewer_panel.hpp"

#include "features/source_preview/source_preview_model.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "UX/theme.hpp"
#include "Widgets/viewer_gl.hpp"
#include "core/timecode.hpp"

namespace canvas::gui::source_preview {

SourceViewerPanel::SourceViewerPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sourceViewerPanel"));

    viewer_ = new ViewerGL(this);
    viewer_->set_mode(ViewerGL::ViewerMode::Source);
    viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* empty = new QWidget(this);
    auto* empty_lay = new QVBoxLayout(empty);
    empty_lay->setContentsMargins(16, 16, 16, 16);
    empty_lay->setSpacing(8);
    // Branded two-tier empty state, matching the Program viewer's voice: a
    // gold title line over a faint hint.
    auto* empty_title = new QLabel(tr("Source preview"), empty);
    empty_title->setAlignment(Qt::AlignCenter);
    apply_theme_style(empty_title, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 14px; font-weight: 650;"
            " background: transparent; }")
            .arg(css(tokens().accent_text));
    });
    auto* empty_hint = new QLabel(
        tr("Hover a media clip in the pool to inspect it,\nor press the play button to audition."),
        empty);
    empty_hint->setAlignment(Qt::AlignCenter);
    empty_hint->setWordWrap(true);
    apply_theme_style(empty_hint, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 11.5px; background: transparent; }")
            .arg(css(tokens().ink_faint));
    });
    empty_lay->addStretch(1);
    empty_lay->addWidget(empty_title);
    empty_lay->addWidget(empty_hint);
    empty_lay->addStretch(1);
    empty_state_ = empty;

    auto* stacked = new QStackedLayout;
    stacked->addWidget(viewer_);
    stacked->addWidget(empty);
    stacked->setCurrentWidget(empty);
    stacked_ = stacked;

    name_label_ = new QLabel(this);
    name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    apply_theme_style(name_label_, [] {
        return QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: 500; "
                              "background: transparent; }")
            .arg(css(tokens().ink));
    });

    time_label_ = new QLabel(tr("00:00:00:00"), this);
    apply_theme_style(time_label_, [] {
        return QStringLiteral("QLabel { color: %1; font-size: 11px; font-family: %2; "
                              "background: transparent; }")
            .arg(css(tokens().ink_muted), css(tokens().font_mono));
    });

    play_button_ = new QToolButton(this);
    play_button_->setIcon(icon("play"));
    play_button_->setIconSize(QSize(16, 16));
    play_button_->setAutoRaise(true);
    play_button_->setToolTip(tr("Play source preview (Space)"));
    apply_theme_style(play_button_, &flat_tool_style);

    mini_scrub_ = new QSlider(Qt::Horizontal, this);
    mini_scrub_->setRange(0, 10000);
    mini_scrub_->setFixedHeight(16);
    mini_scrub_->setSingleStep(1);
    apply_theme_style(mini_scrub_, &slider_style);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    header->addWidget(name_label_, 1);
    header->addWidget(time_label_);
    header->addWidget(play_button_);
    header->addWidget(mini_scrub_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);
    layout->addLayout(header);
    layout->addLayout(stacked, 1);

    connect(play_button_, &QToolButton::clicked, this, &SourceViewerPanel::play_clicked);

    connect(mini_scrub_, &QSlider::sliderPressed, this,
            [this] { slider_down_ = true; });
    connect(mini_scrub_, &QSlider::sliderReleased, this, [this] {
        slider_down_ = false;
        emit scrub_fraction(mini_scrub_->value() / 10000.0);
    });
    connect(mini_scrub_, &QSlider::valueChanged, this, [this](int value) {
        if (slider_down_) emit scrub_fraction(value / 10000.0);
    });
}

void SourceViewerPanel::set_media_info(const QString& name, const bool is_video,
                                       const bool is_audio, const int64_t total_frames) {
    total_frames_ = total_frames;
    name_label_->setText(name.isEmpty() ? tr("(unnamed)") : name);
    if (total_frames_ > 0) {
        QSignalBlocker b(mini_scrub_);
        mini_scrub_->setValue(0);
    }
    stacked_->setCurrentWidget(viewer_);
}

void SourceViewerPanel::set_media_position(const int64_t frame, const double fps) {
    update_time_label(frame, fps);
    if (!slider_down_ && total_frames_ > 1) {
        QSignalBlocker b(mini_scrub_);
        const double frac = frame_to_fraction(frame, total_frames_);
        mini_scrub_->setValue(static_cast<int>(std::lround(frac * 10000.0)));
    }
}

// Format the position as HH:MM:SS:FF at the media's frame rate (the shared
// gui timecode law).
void SourceViewerPanel::update_time_label(const int64_t frame, const double fps) {
    time_label_->setText(fps > 0.0
                             ? timecode(std::max<int64_t>(0, frame), fps)
                             : QStringLiteral("00:00:00:00"));
}

void SourceViewerPanel::set_playing(const bool playing) {
    viewer_->set_playing(playing);
    play_button_->setIcon(icon(playing ? "pause" : "play"));
    play_button_->setToolTip(playing ? tr("Pause source preview (Space)")
                                     : tr("Play source preview (Space)"));
}

void SourceViewerPanel::clear_media() {
    total_frames_ = 0;
    name_label_->clear();
    update_time_label(0, 0.0);
    stacked_->setCurrentWidget(empty_state_);
    play_button_->setIcon(icon("play"));
}

}  // namespace canvas::gui::source_preview