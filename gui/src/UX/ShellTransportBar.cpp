// Playback transport bar: viewport/frame selector far left, in/out-navigation
// + the centered transport cluster (Start/Prev/Stop/Play/Next/End/Loop), snap
// and edge-jump group far right, then the time label and overview scrub slider.
// Split out of ShellTopBar.cpp (splitplan refactor).

#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

#include "Widgets/viewport_selector.hpp"
#include "core/timecode.hpp"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSize>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

namespace canvas::gui {

QWidget* build_transport_bar(MainWindow& mw) {
    // Transport bar (playback): flat icon cluster centered, in/out nav just
    // left of it, viewport selector far left, snap/next/edge group far right.
    auto flat_btn = [&mw](const QIcon& ic, const char* tip, bool checkable) {
        auto* b = new QToolButton(&mw);
        b->setIcon(ic);
        b->setIconSize(QSize(18, 18));
        b->setToolTip(MainWindow::tr(tip));
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        apply_theme_style(b, &flat_tool_style);
        return b;
    };

    auto* transport = new QWidget(&mw);
    transport->setObjectName(QStringLiteral("transportBar"));
    apply_theme_style(transport, &transport_bar_style);
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(12, 6, 12, 6);
    transport_layout->setSpacing(8);

    // Far left: viewport/frame selector — fully hand-painted widget (size, label,
    // chevron, popup), stacks on top of the transport bar so nothing clips it.
    auto* viewport_select = new ViewportSelector(transport);
    viewport_select->raise();
    transport_layout->addWidget(viewport_select);
    transport_layout->addStretch(1);

    // In/out-point nav + centered transport cluster.
    auto* center = new QHBoxLayout;
    center->setSpacing(6);
    center->addWidget(flat_btn(icon("chevron_left"), "Previous edit point", false));
    center->addWidget(flat_btn(icon("mark_in"), "Mark In (I)", false));
    center->addWidget(flat_btn(icon("mark_out"), "Mark Out (O)", false));
    center->addWidget(flat_btn(icon("chevron_right"), "Next edit point", false));
    center->addSpacing(12);

    auto* to_start = flat_btn(icon("to_start"), "Go to Start (Home)", false);
    auto* prev_frame = flat_btn(icon("step_back"), "Previous Frame (Left)", false);
    auto* stop_btn = flat_btn(icon("stop"), "Stop", false);
    // Play/Pause is a standard transport button like every other control here:
    // same flat semi-rounded surface, same theme-tinted glyph, no hero disc.
    mw.play_button_ = flat_btn(icon("play"), "Play/Pause (Space)", false);
    auto* next_frame = flat_btn(icon("step_forward"), "Next Frame (Right)", false);
    auto* to_end = flat_btn(icon("to_end"), "Go to End (End)", false);
    auto* loop_btn = flat_btn(icon("loop"), "Loop playback", true);
    for (auto* w : {static_cast<QWidget*>(to_start), static_cast<QWidget*>(prev_frame),
                    static_cast<QWidget*>(stop_btn), static_cast<QWidget*>(mw.play_button_),
                    static_cast<QWidget*>(next_frame), static_cast<QWidget*>(to_end),
                    static_cast<QWidget*>(loop_btn)}) {
        center->addWidget(w);
    }
    transport_layout->addLayout(center);
    transport_layout->addStretch(1);

    // Far right: snap / next-edit / edge-jump grouped tightly.
    auto* right_group = new QHBoxLayout;
    right_group->setSpacing(6);
    auto* snap_btn = flat_btn(icon("snap"), "Snap", true);
    snap_btn->setChecked(true);
    right_group->addWidget(snap_btn);
    right_group->addWidget(flat_btn(icon("next_edit"), "Next edit", false));
    right_group->addWidget(flat_btn(icon("edge_jump"), "Jump to edge", false));
    transport_layout->addLayout(right_group);
    transport_layout->addSpacing(8);

    mw.time_label_ = new QLabel(timecode(-1, 0.0), &mw);
    mw.time_label_->setObjectName(QStringLiteral("timeLabel"));
    apply_theme_style(mw.time_label_, &time_label_style);
    transport_layout->addWidget(mw.time_label_);

    mw.scrub_ = new QSlider(Qt::Horizontal, &mw);
    mw.scrub_->setRange(0, 0);
    apply_theme_style(mw.scrub_, &slider_style);

    // ---- Transport connects ----
    // Snap toggle drives the timeline magnetism (clip drags, trim, playhead);
    // it defaults ON and the button is pre-checked to match.
    QObject::connect(snap_btn, &QToolButton::clicked, &mw, [&mw](bool checked) {
        mw.timeline_->set_snap_enabled(checked);
    });
    // Explicit playhead jumps re-enable playhead-follow (a user who scrolled
    // away during playback wants the playhead centered again after a jump).
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
        // Stop = pause with the playhead left in place (the standard transport stop).
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

    // Scrubbing: re-position with a fast low-res preview while dragging; commit
    // the crisp full-res frame on release. Playback keeps running throughout, but
    // scrubbing marks the drag so previews don't rewind the live audio pipe per
    // move (only once, on release via seek()).
    QObject::connect(mw.scrub_, &QSlider::sliderPressed, &mw, [&mw] {
        mw.timeline_->set_follow_playhead(true);
        mw.controller_.begin_scrub();
    });
    // The overview slider snaps to the same cut points as the timeline ruler
    // (clip edges / bookmarks within the magnet radius, then the grid).
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

}  // namespace canvas::gui