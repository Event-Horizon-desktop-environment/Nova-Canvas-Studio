#include "UX/MainWindow.hpp"

#include <QAction>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include "Widgets/viewport_selector.hpp"
#include "core/timecode.hpp"
#include "UX/theme.hpp"

namespace canvas::gui {

QWidget* build_top_bar(MainWindow& mw) {
    auto* top_bar = new QWidget(&mw);
    top_bar->setObjectName(QStringLiteral("topStatusBar"));
    apply_theme_style(top_bar, &top_status_bar_style);
    auto* top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(10, 4, 10, 4);
    top_bar_layout->setSpacing(10);

    auto* format_label = new QLabel(MainWindow::tr("1440p"), top_bar);
    apply_theme_style(format_label, [] { return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_muted)); });
    auto* edited_label = new QLabel(QStringLiteral("\u00B7 ") + MainWindow::tr("Edited"), top_bar);
    apply_theme_style(edited_label, [] { return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint)); });
    mw.fps_label_ = new QLabel(MainWindow::tr("0 fps"), top_bar);
    apply_theme_style(mw.fps_label_, [] { return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint)); });
    top_bar_layout->addWidget(format_label);
    top_bar_layout->addWidget(edited_label);
    auto* fps_sep = new QLabel(QStringLiteral("\u00B7"), top_bar);
    apply_theme_style(fps_sep, [] { return QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint)); });
    top_bar_layout->addWidget(fps_sep);
    top_bar_layout->addWidget(mw.fps_label_);
    top_bar_layout->addStretch(1);

    auto* top_timecode = new QLabel(timecode(0, 0.0), top_bar);
    top_timecode->setObjectName(QStringLiteral("topTimecode"));
    apply_theme_style(top_timecode, &big_timecode_style);
    top_bar_layout->addWidget(top_timecode);
    top_bar_layout->addStretch(1);

    auto* quick_export_btn = new QToolButton(top_bar);
    quick_export_btn->setText(MainWindow::tr("Quick Export"));
    auto* fullscreen_top_btn = new QToolButton(top_bar);
    fullscreen_top_btn->setText(MainWindow::tr("Full Screen"));
    QObject::connect(fullscreen_top_btn, &QToolButton::clicked, &mw,
            [&mw] { mw.isFullScreen() ? mw.showNormal() : mw.showFullScreen(); });
    auto* mixer_btn = new QToolButton(top_bar);
    mixer_btn->setText(MainWindow::tr("Mixer"));
    mixer_btn->setCheckable(true);
    auto* metadata_btn = new QToolButton(top_bar);
    metadata_btn->setText(MainWindow::tr("Metadata"));
    metadata_btn->setCheckable(true);
    auto* inspector_top_btn = new QToolButton(top_bar);
    inspector_top_btn->setText(MainWindow::tr("Inspector"));
    inspector_top_btn->setCheckable(true);
    mw.inspector_top_btn_ = inspector_top_btn;
    // Wire the button back to the Inspector action + dock: both were built
    // earlier (menu action / InspDock), so all three objects are alive here.
    // (Connecting in build_inspector_dock used to hit a still-null button.)
    QObject::connect(mw.inspector_toggle_action_, &QAction::toggled, mw.inspector_top_btn_,
                     &QToolButton::setChecked);
    QObject::connect(mw.inspector_top_btn_, &QToolButton::toggled, &mw,
            [&mw](bool on) {
                mw.inspector_toggle_action_->setChecked(on);
                if (mw.inspector_dock_) mw.inspector_dock_->setVisible(on);
            });
    for (auto* b : {quick_export_btn, fullscreen_top_btn, mixer_btn, metadata_btn, inspector_top_btn}) {
        b->setAutoRaise(true);
        apply_theme_style(b, &page_pill_style);
        top_bar_layout->addWidget(b);
    }

    QObject::connect(&mw.controller_, &SequenceController::position_changed, &mw,
            [top_timecode, &mw](int64_t frame) {
                top_timecode->setText(timecode(frame, mw.fps_));
            });

    return top_bar;
}

void build_page_bar(MainWindow& mw) {
    auto* page_bar = new QToolBar(MainWindow::tr("Pages"), &mw);
    page_bar->setMovable(false);
    page_bar->setObjectName(QStringLiteral("pageSwitcher"));
    apply_theme_style(page_bar, &page_switcher_style);
    page_bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    page_bar->setIconSize(QSize(16, 16));

    // Stretchable spacer so the page buttons sit centered in the bar.
    auto* page_bar_spacer_l = new QWidget(page_bar);
    page_bar_spacer_l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    page_bar->addWidget(page_bar_spacer_l);

    const char* page_names[] = {"Media", "Cut", "Edit", "Fusion", "Color", "Fairlight", "Deliver"};
    for (const char* name : page_names) {
        auto* b = new QToolButton(page_bar);
        const bool is_edit = qstrcmp(name, "Edit") == 0;
        b->setText(MainWindow::tr(name));
        b->setCheckable(true);
        b->setChecked(is_edit);
        b->setToolTip(MainWindow::tr("%1 page").arg(MainWindow::tr(name)));
        b->setAutoRaise(true);
        apply_theme_style(b, &page_pill_style);
        page_bar->addWidget(b);
        QObject::connect(b, &QToolButton::clicked, &mw, [&mw, b, name, page_bar](bool) {
            // Only Edit and Deliver have distinct layouts right now; the rest
            // fall back to the Edit workspace.
            const bool deliver = qstrcmp(name, "Deliver") == 0;
            for (QToolButton* other : page_bar->findChildren<QToolButton*>()) {
                if (other != b) other->setChecked(false);
            }
            b->setChecked(true);
            if (deliver) mw.enter_deliver_page();
            else mw.enter_edit_page();
        });
    }

    // Second stretchable spacer: centers the page group and pushes the
    // Home/Settings cluster to the far right of the bar.
    auto* page_bar_spacer_r = new QWidget(page_bar);
    page_bar_spacer_r->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    page_bar->addWidget(page_bar_spacer_r);

    auto* home_btn = new QToolButton(page_bar);
    home_btn->setText(MainWindow::tr("Home"));
    home_btn->setAutoRaise(true);
    apply_theme_style(home_btn, &page_pill_style);
    auto* settings_btn = new QToolButton(page_bar);
    settings_btn->setIcon(icon("settings"));
    settings_btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    settings_btn->setAutoRaise(true);
    apply_theme_style(settings_btn, &flat_tool_style);
    page_bar->addWidget(home_btn);
    page_bar->addWidget(settings_btn);
    mw.addToolBar(Qt::BottomToolBarArea, page_bar);
}

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