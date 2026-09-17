#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

#include "Widgets/viewport_selector.hpp"
#include "core/timecode.hpp"
#include "features/timeline/view_options_menu.hpp"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

namespace canvas::gui {

QWidget* build_transport_bar(MainWindow& mw) {
    auto btn = [&mw](const QIcon& ic, const char* tip, bool checkable) {
        auto* b = new QToolButton(&mw);
        b->setIcon(ic);
        b->setIconSize(QSize(18, 18));
        b->setToolTip(MainWindow::tr(tip));
        b->setCheckable(checkable);
        apply_theme_style(b, &transport_tool_style);
        return b;
    };

    auto* transport = new QWidget(&mw);
    transport->setObjectName(QStringLiteral("transportBar"));
    apply_theme_style(transport, &transport_bar_style);
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(12, 6, 12, 6);
    transport_layout->setSpacing(8);

    auto* left_flank = new QWidget(transport);
    left_flank->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* left_flank_layout = new QHBoxLayout(left_flank);
    left_flank_layout->setContentsMargins(0, 0, 0, 0);
    left_flank_layout->setSpacing(0);
    auto* viewport_select = new ViewportSelector(left_flank);
    left_flank_layout->addWidget(viewport_select);
    viewport_select->raise();

    auto* center = new QHBoxLayout;
    center->setSpacing(6);
    center->addWidget(btn(icon("chevron_left"), "Previous edit point", false));
    center->addWidget(btn(icon("mark_in"), "Mark In (I)", false));
    center->addWidget(btn(icon("mark_out"), "Mark Out (O)", false));
    center->addWidget(btn(icon("chevron_right"), "Next edit point", false));
    center->addSpacing(12);

    auto* to_start = btn(icon("to_start"), "Go to Start (Home)", false);
    auto* prev_frame = btn(icon("step_back"), "Previous Frame (Left)", false);
    auto* stop_btn = btn(icon("stop"), "Stop", false);
    mw.play_button_ = btn(icon("play"), "Play/Pause (Space)", false);
    auto* next_frame = btn(icon("step_forward"), "Next Frame (Right)", false);
    auto* to_end = btn(icon("to_end"), "Go to End (End)", false);
    auto* loop_btn = btn(icon("loop"), "Loop playback", true);
    for (auto* w : {static_cast<QWidget*>(to_start), static_cast<QWidget*>(prev_frame),
                    static_cast<QWidget*>(stop_btn), static_cast<QWidget*>(mw.play_button_),
                    static_cast<QWidget*>(next_frame), static_cast<QWidget*>(to_end),
                    static_cast<QWidget*>(loop_btn)}) {
        center->addWidget(w);
    }

    auto* right_flank = new QWidget(transport);
    right_flank->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* right_flank_layout = new QHBoxLayout(right_flank);
    right_flank_layout->setContentsMargins(0, 0, 0, 0);
    right_flank_layout->setSpacing(6);
    auto* view_opts_btn = btn(icon("TimeLine-View-option"), "Timeline view options", false);
    attach_timeline_view_options_button(mw, view_opts_btn);
    right_flank_layout->addWidget(view_opts_btn);
    auto* snap_btn = btn(icon("snap"), "Snap", true);
    snap_btn->setChecked(true);
    right_flank_layout->addWidget(snap_btn);
    right_flank_layout->addWidget(btn(icon("next_edit"), "Next edit", false));
    right_flank_layout->addWidget(btn(icon("edge_jump"), "Jump to edge", false));
    right_flank_layout->addSpacing(8);
    mw.time_label_ = new QLabel(timecode(-1, 0.0), right_flank);
    mw.time_label_->setObjectName(QStringLiteral("timeLabel"));
    apply_theme_style(mw.time_label_, &time_label_style);
    right_flank_layout->addWidget(mw.time_label_);

    left_flank->setMinimumWidth(right_flank->sizeHint().width());

    transport_layout->addWidget(left_flank);
    transport_layout->addStretch(1);
    transport_layout->addLayout(center);
    transport_layout->addStretch(1);
    transport_layout->addWidget(right_flank);

    mw.scrub_ = new QSlider(Qt::Horizontal, &mw);
    mw.scrub_->setRange(0, 0);
    apply_theme_style(mw.scrub_, &slider_style);

    QObject::connect(snap_btn, &QToolButton::clicked, &mw, [&mw](bool checked) {
        mw.timeline_->set_snap_enabled(checked);
    });
    QObject::connect(to_start, &QToolButton::clicked, &mw, [&mw] {
        mw.timeline_->set_follow_playhead(true);
        mw.controller_.seek(0);
    });
    QObject::connect(prev_frame, &QToolButton::clicked, &mw, [&mw] {
        mw.controller_.pause();
        mw.controller_.step(-1);
    });
    QObject::connect(mw.play_button_, &QToolButton::clicked, &mw, [&mw] { mw.controller_.toggle_play_pause(); });
    QObject::connect(stop_btn, &QToolButton::clicked, &mw, [&mw] {
        mw.controller_.pause();
    });
    QObject::connect(next_frame, &QToolButton::clicked, &mw, [&mw] {
        mw.controller_.pause();
        mw.controller_.step(1);
    });
    QObject::connect(to_end, &QToolButton::clicked, &mw, [&mw] {
        mw.timeline_->set_follow_playhead(true);
        mw.controller_.seek(mw.total_frames_ - 1);
    });

    QObject::connect(mw.scrub_, &QSlider::sliderPressed, &mw, [&mw] {
        mw.timeline_->set_follow_playhead(true);
        mw.controller_.begin_scrub();
    });
    QObject::connect(mw.scrub_, &QSlider::sliderMoved, &mw, [&mw](int value) {
        mw.controller_.seek_preview(mw.timeline_->snap_frame(value));
    });
    QObject::connect(mw.scrub_, &QSlider::sliderReleased, &mw, [&mw] {
        mw.controller_.end_scrub();
        mw.controller_.seek(mw.timeline_->snap_frame(mw.scrub_->value()));
    });
    QObject::connect(mw.scrub_, &QSlider::valueChanged, &mw, [&mw](int) { mw.update_time_label(); });

    return transport;
}

}
