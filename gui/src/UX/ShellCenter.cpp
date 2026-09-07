#include "UX/MainWindow.hpp"
#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "UX/InspectorTransition.hpp"
#include "UX/InspectorVisual.hpp"
#include "ui_MainWindow.h"

#include <QAction>
#include <QDockWidget>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStandardPaths>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "UX/theme.hpp"
#include "Widgets/timeline_widget.hpp"
#include "Widgets/viewer_gl.hpp"
#include "core/timecode.hpp"

namespace canvas::gui {

void build_center_workspace(MainWindow& mw) {
    // ---- 6. CENTER — the viewer with its contextual toolbar ----
    mw.viewer_ = new ViewerGL(&mw);

    // Fit/Fill monitor scaling, available via right-click on the viewer:
    // Fit shows the whole frame with letterbox bars (default),
    // Fill crops to cover the media window edge-to-edge.
    mw.viewer_->setContextMenuPolicy(Qt::CustomContextMenu);
    const bool saved_scale = QSettings().value(QStringLiteral("viewerScaleFill"), false).toBool();
    mw.viewer_->set_scale_mode(saved_scale ? ViewerGL::ScaleMode::Fill : ViewerGL::ScaleMode::Fit);
    QObject::connect(mw.viewer_, &QWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu(MainWindow::tr("Monitor Scale"), &mw);
                QAction* fit = menu.addAction(MainWindow::tr("Fit (letterbox)"));
                fit->setCheckable(true);
                fit->setChecked(mw.viewer_->scale_mode() == ViewerGL::ScaleMode::Fit);
                QAction* fill = menu.addAction(MainWindow::tr("Fill (crop to window)"));
                fill->setCheckable(true);
                fill->setChecked(mw.viewer_->scale_mode() == ViewerGL::ScaleMode::Fill);
                QObject::connect(fit, &QAction::toggled, &mw, [&mw, fill](bool on) {
                    if (!on) return;
                    fill->setChecked(false);
                    mw.viewer_->set_scale_mode(ViewerGL::ScaleMode::Fit);
                    QSettings().setValue(QStringLiteral("viewerScaleFill"), false);
                });
                QObject::connect(fill, &QAction::toggled, &mw, [&mw, fit](bool on) {
                    if (!on) return;
                    fit->setChecked(false);
                    mw.viewer_->set_scale_mode(ViewerGL::ScaleMode::Fill);
                    QSettings().setValue(QStringLiteral("viewerScaleFill"), true);
                });
                menu.exec(mw.viewer_->mapToGlobal(pos));
            });

    // Contextual editing toolbar — sits directly under the viewer, matching
    // the timeline toolbar icon row: tool cluster, marker
    // cluster, then a right-aligned zoom cluster.
    auto* contextual_bar = new QToolBar(MainWindow::tr("Editing Tools"), &mw);
    contextual_bar->setMovable(false);
    contextual_bar->setObjectName(QStringLiteral("contextualTools"));
    contextual_bar->setStyleSheet(timeline_tools_style());
    contextual_bar->setIconSize(QSize(16, 16));

    // Flat icon tool button factory: no persistent border/background; state is
    // shown by a soft highlight box only (checked = active). Manual icon tint.
    auto make_tool = [&mw](QToolBar* bar, const char* icon_name, const char* tip,
                           bool checkable) -> QToolButton* {
        auto* b = new QToolButton(bar);
        b->setIcon(icon(icon_name));
        b->setIconSize(QSize(16, 16));
        b->setToolTip(MainWindow::tr(tip));
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        b->setStyleSheet(flat_tool_style());
        bar->addWidget(b);
        return b;
    };

    // --- Left: utility toggles (menu, snap, mic) — tight, left-aligned.
    make_tool(contextual_bar, "menu", "Application menu", false);
    auto* snap_toggle = make_tool(contextual_bar, "snap", "Snap (toggle)", true);
    snap_toggle->setChecked(true);
    make_tool(contextual_bar, "mic", "Record/take mic", false);

    contextual_bar->addSeparator();

    // --- Center-left: edit tools. Select cursor separate; trim/blade/mode form
    //     a "bracket-pill" outlined group; then link/lock toggles.
    auto* tool_select = new QToolButton(contextual_bar);
    tool_select->setIcon(icon("select"));
    tool_select->setIconSize(QSize(16, 16));
    tool_select->setToolTip(MainWindow::tr("Selection (A)"));
    tool_select->setCheckable(true);
    tool_select->setChecked(true);
    tool_select->setAutoRaise(true);
    tool_select->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(tool_select);

    auto* tool_trim = new QToolButton(contextual_bar);
    tool_trim->setIcon(icon("trim"));
    tool_trim->setIconSize(QSize(16, 16));
    tool_trim->setToolTip(MainWindow::tr("Trim (T)"));
    tool_trim->setCheckable(true);
    tool_trim->setAutoRaise(true);
    tool_trim->setStyleSheet(tool_cluster_style());
    contextual_bar->addWidget(tool_trim);
    auto* tool_blade = new QToolButton(contextual_bar);
    tool_blade->setIcon(icon("razor_blade"));
    tool_blade->setIconSize(QSize(16, 16));
    tool_blade->setToolTip(MainWindow::tr("Blade (B)"));
    tool_blade->setCheckable(true);
    tool_blade->setAutoRaise(true);
    tool_blade->setStyleSheet(tool_cluster_style());
    contextual_bar->addWidget(tool_blade);
    auto* tool_mode = new QToolButton(contextual_bar);
    tool_mode->setIcon(icon("mode"));
    tool_mode->setIconSize(QSize(16, 16));
    tool_mode->setToolTip(MainWindow::tr("Edit mode"));
    tool_mode->setCheckable(true);
    tool_mode->setAutoRaise(true);
    tool_mode->setStyleSheet(tool_cluster_style());
    contextual_bar->addWidget(tool_mode);

    contextual_bar->addSeparator();
    auto* linked_sel = new QToolButton(contextual_bar);
    linked_sel->setIcon(icon("linked_sel"));
    linked_sel->setIconSize(QSize(16, 16));
    linked_sel->setCheckable(true);
    linked_sel->setChecked(true);
    linked_sel->setToolTip(MainWindow::tr("Linked selection"));
    linked_sel->setAutoRaise(true);
    linked_sel->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(linked_sel);
    auto* sync_lock = new QToolButton(contextual_bar);
    sync_lock->setIcon(icon("sync_lock"));
    sync_lock->setIconSize(QSize(16, 16));
    sync_lock->setCheckable(true);
    sync_lock->setToolTip(MainWindow::tr("Track lock"));
    sync_lock->setAutoRaise(true);
    sync_lock->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(sync_lock);

    contextual_bar->addSeparator();

    // Two accent dropdown badges — the only saturated elements in the toolbar.
    make_tool(contextual_bar, "color_tag", "Track color", false);
    auto* marker_color = new QToolButton(contextual_bar);
    marker_color->setIcon(icon("mark_in", QColor(0xC9, 0x86, 0x3A)));
    marker_color->setIconSize(QSize(14, 14));
    marker_color->setToolTip(MainWindow::tr("Marker color"));
    marker_color->setAutoRaise(true);
    marker_color->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(marker_color);
    auto* marker_down = new QToolButton(contextual_bar);
    marker_down->setIcon(icon("chevron_down", QColor(0xC9, 0x86, 0x3A)));
    marker_down->setToolTip(MainWindow::tr("Marker color"));
    marker_down->setAutoRaise(true);
    marker_down->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(marker_down);

    auto* bar_spacer = new QWidget(contextual_bar);
    bar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    contextual_bar->addWidget(bar_spacer);

    // --- Right: zoom controls (fit, zoom, slider, volume, slider, DIM).
    auto* zoom_fit = new QToolButton(contextual_bar);
    zoom_fit->setIcon(icon("fit"));
    zoom_fit->setIconSize(QSize(16, 16));
    zoom_fit->setToolTip(MainWindow::tr("Zoom to fit"));
    zoom_fit->setAutoRaise(true);
    zoom_fit->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(zoom_fit);
    auto* zoom_out = new QToolButton(contextual_bar);
    zoom_out->setIcon(icon("zoom_out"));
    zoom_out->setIconSize(QSize(16, 16));
    zoom_out->setAutoRaise(true);
    zoom_out->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(zoom_out);
    auto* zoom_slider = new QSlider(Qt::Horizontal, contextual_bar);
    zoom_slider->setRange(0, 100);
    zoom_slider->setValue(33); // 100% baseline of the 50%..200% band
    zoom_slider->setFixedWidth(140);
    zoom_slider->setStyleSheet(slider_style());
    contextual_bar->addWidget(zoom_slider);
    auto* zoom_in = new QToolButton(contextual_bar);
    zoom_in->setIcon(icon("zoom_in"));
    zoom_in->setIconSize(QSize(16, 16));
    zoom_in->setAutoRaise(true);
    zoom_in->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(zoom_in);

    auto* volume_icon = new QToolButton(contextual_bar);
    volume_icon->setIcon(icon("volume"));
    volume_icon->setIconSize(QSize(16, 16));
    volume_icon->setToolTip(MainWindow::tr("Monitoring volume"));
    volume_icon->setAutoRaise(true);
    volume_icon->setStyleSheet(flat_tool_style());
    contextual_bar->addWidget(volume_icon);
    auto* volume_slider = new QSlider(Qt::Horizontal, contextual_bar);
    volume_slider->setRange(0, 100);
    volume_slider->setValue(80);
    volume_slider->setFixedWidth(90);
    volume_slider->setStyleSheet(slider_style());
    contextual_bar->addWidget(volume_slider);

    auto* dim_btn = new QToolButton(contextual_bar);
    dim_btn->setText(MainWindow::tr("DIM"));
    dim_btn->setToolTip(MainWindow::tr("Temporarily dip monitoring volume"));
    dim_btn->setAutoRaise(true);
    dim_btn->setCheckable(true);
    dim_btn->setStyleSheet(outline_pill_style());
    contextual_bar->addWidget(dim_btn);

    const auto sync_zoom_slider = [&mw, zoom_slider, zoom_in, zoom_out] {
        if (!mw.timeline_) return;
        const double pct =
            std::clamp(mw.timeline_->zoom_percent(), TimelineWidget::kZoomMinPercent,
                       TimelineWidget::kZoomMaxPercent);
        const int v = static_cast<int>(std::round(
            (pct - TimelineWidget::kZoomMinPercent) /
            (TimelineWidget::kZoomMaxPercent - TimelineWidget::kZoomMinPercent) * 100.0));
        QSignalBlocker blocker(zoom_slider);
        zoom_slider->setValue(std::clamp(v, 0, 100));
        const int show_pct = static_cast<int>(std::round(pct));
        zoom_in->setToolTip(MainWindow::tr("Zoom in (%1%)").arg(show_pct));
        zoom_out->setToolTip(MainWindow::tr("Zoom out (%1%)").arg(show_pct));
    };

    QObject::connect(zoom_fit, &QToolButton::clicked, &mw, [&mw, sync_zoom_slider] {
        mw.timeline_->zoom_fit();
        sync_zoom_slider();
    });
    QObject::connect(zoom_out, &QToolButton::clicked, &mw, [&mw, sync_zoom_slider] {
        mw.timeline_->zoom_out();
        sync_zoom_slider();
    });
    QObject::connect(zoom_in, &QToolButton::clicked, &mw, [&mw, sync_zoom_slider] {
        mw.timeline_->zoom_in();
        sync_zoom_slider();
    });
    QObject::connect(zoom_slider, &QSlider::valueChanged, &mw, [&mw](int v) {
        const double pct =
            TimelineWidget::kZoomMinPercent +
            (TimelineWidget::kZoomMaxPercent - TimelineWidget::kZoomMinPercent) * v / 100.0;
        mw.timeline_->set_zoom_percent(pct);
    });

    sync_zoom_slider();

    // Monitoring volume: the slider sets the level (0..100 -> 0..1); clicking
    // the volume icon toggles mute/unmute and swaps the icon glyph.
    QObject::connect(volume_slider, &QSlider::valueChanged, &mw,
            [&mw](int v) { mw.controller_.set_volume(v / 100.0f); });
    auto update_volume_icon = [&mw, volume_icon, volume_slider] {
        volume_icon->setIcon(mw.controller_.muted() ? icon("mute") : icon("volume"));
        volume_icon->setToolTip(mw.controller_.muted() ? MainWindow::tr("Unmute monitoring volume")
                                                      : MainWindow::tr("Mute monitoring volume"));
        if (!mw.controller_.muted() && !volume_slider->isSliderDown())
            volume_slider->setValue(static_cast<int>(mw.controller_.volume() * 100.0f + 0.5f));
    };
    QObject::connect(volume_icon, &QToolButton::clicked, &mw, [&mw, update_volume_icon] {
        mw.controller_.set_muted(!mw.controller_.muted());
        update_volume_icon();
    });
    update_volume_icon();
    // DIM: toggle dips monitoring volume to a fixed lower level (see AudioOutput).
    QObject::connect(dim_btn, &QToolButton::toggled, &mw, [&mw, dim_btn](bool on) {
        mw.controller_.set_dimmed(on);
        dim_btn->setToolTip(on ? MainWindow::tr("Monitoring volume dimmed (click to restore)")
                               : MainWindow::tr("Temporarily dip monitoring volume"));
    });
    QObject::connect(snap_toggle, &QToolButton::toggled, &mw, [&mw](bool on) { mw.timeline_->set_snap_enabled(on); });
    auto set_tool = [&mw, tool_select, tool_trim, tool_blade](QToolButton* target, TimelineWidget::Tool tool) {
        for (auto* b : {tool_select, tool_trim, tool_blade}) b->setChecked(b == target);
        mw.timeline_->set_tool(tool);
    };
    QObject::connect(tool_select, &QToolButton::clicked, &mw,
            [set_tool, tool_select] { set_tool(tool_select, TimelineWidget::Tool::Select); });
    QObject::connect(tool_trim, &QToolButton::clicked, &mw,
            [set_tool, tool_trim] { set_tool(tool_trim, TimelineWidget::Tool::Trim); });
    QObject::connect(tool_blade, &QToolButton::clicked, &mw,
            [set_tool, tool_blade] { set_tool(tool_blade, TimelineWidget::Tool::Blade); });

    // Viewer bounding frame.
    auto* viewer_frame = new QFrame(&mw);
    viewer_frame->setObjectName(QStringLiteral("viewerFrame"));
    viewer_frame->setStyleSheet(viewer_frame_style());
    auto* viewer_frame_layout = new QVBoxLayout(viewer_frame);
    viewer_frame_layout->setContentsMargins(6, 6, 6, 6);
    viewer_frame_layout->addWidget(mw.viewer_, 1);

    // Transport bar (playback) + overview scrub slider — ShellTopBar.cpp.
    auto* transport = build_transport_bar(mw);

    // Top status bar — ShellTopBar.cpp (needs to be a child of the column so the
    // viewer column can stack it above the monitor).
    auto* top_bar = build_top_bar(mw);

    // Assemble the viewer column: top scrub bar > top status bar > viewer(frame)
    // > contextual tools > transport.
    auto* viewer_column = new QWidget(&mw);
    auto* viewer_layout = new QVBoxLayout(viewer_column);
    viewer_layout->setContentsMargins(0, 0, 0, 0);
    viewer_layout->setSpacing(0);

    // Thin full-width overview scrub bar at the very top: small playhead marker
    // on the left, the overview scrubber in the middle, a lock icon far right.
    auto* top_scrub = new QWidget(viewer_column);
    top_scrub->setObjectName(QStringLiteral("topScrubBar"));
    top_scrub->setStyleSheet(QStringLiteral(
        "QWidget#topScrubBar { background: #11131A; border-bottom: 1px solid #232833; }"));
    auto* top_scrub_layout = new QHBoxLayout(top_scrub);
    top_scrub_layout->setContentsMargins(10, 4, 10, 4);
    top_scrub_layout->setSpacing(8);
    auto* scrub_marker = new QToolButton(top_scrub);
    scrub_marker->setIcon(icon("play"));
    scrub_marker->setIconSize(QSize(12, 12));
    scrub_marker->setAutoRaise(true);
    scrub_marker->setStyleSheet(flat_tool_style());
    scrub_marker->setToolTip(MainWindow::tr("Playhead"));
    top_scrub_layout->addWidget(scrub_marker);
    top_scrub_layout->addWidget(mw.scrub_, 1);
    auto* scrub_lock = new QToolButton(top_scrub);
    scrub_lock->setIcon(icon("lock"));
    scrub_lock->setIconSize(QSize(16, 16));
    scrub_lock->setCheckable(true);
    scrub_lock->setAutoRaise(true);
    scrub_lock->setStyleSheet(flat_tool_style());
    scrub_lock->setToolTip(MainWindow::tr("Lock timeline"));
    top_scrub_layout->addWidget(scrub_lock);
    viewer_layout->addWidget(top_scrub);
    viewer_layout->addWidget(top_bar);
    auto* viewer_inner = new QWidget(viewer_column);
    auto* viewer_inner_layout = new QVBoxLayout(viewer_inner);
    viewer_inner_layout->setContentsMargins(8, 6, 8, 6);
    viewer_inner_layout->setSpacing(4);
    viewer_inner_layout->addWidget(viewer_frame, 1);
    viewer_inner_layout->addWidget(contextual_bar);
    viewer_inner_layout->addWidget(transport);
    viewer_layout->addWidget(viewer_inner, 1);

    // ---- 7. TIMELINE — locked to the bottom of the window ----
    mw.timeline_ = new TimelineWidget(&mw);
    mw.timeline_->set_sequence(&mw.project_->sequence);
    mw.timeline_->set_fps(mw.project_->sequence.fps);
    mw.timeline_->set_thumbnail_service(&mw.thumbnails_);
    mw.thumbnails_.set_cache_dir(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("thumbs")));
    QObject::connect(mw.timeline_, &TimelineWidget::media_dropped, &mw,
            [&mw](int media_id, int64_t frame) { mw.place_media_at(media_id, frame, canvas::core::Placement::Overwrite); });

    sync_zoom_slider();

    auto* timeline_frame = new QFrame(&mw);
    timeline_frame->setObjectName(QStringLiteral("timelineFrame"));
    timeline_frame->setStyleSheet(timeline_frame_style());
    auto* timeline_frame_layout = new QVBoxLayout(timeline_frame);
    timeline_frame_layout->setContentsMargins(6, 6, 6, 4);
    timeline_frame_layout->addWidget(mw.timeline_, 1);

    auto* timeline_dock = mw.ui->timelineDock;
    timeline_dock->setObjectName(QStringLiteral("timelineDock"));
    auto* timeline_title = new QWidget(timeline_dock);
    timeline_title->setObjectName(QStringLiteral("timelineDockTitle"));
    timeline_title->setStyleSheet(QStringLiteral(
        "QWidget#timelineDockTitle { background-color: #1A1D27;"
        " border-bottom: 1px solid #232833; }"));
    timeline_dock->setTitleBarWidget(timeline_title);
    timeline_dock->setWidget(timeline_frame);
    timeline_dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    timeline_dock->setMinimumHeight(140);
    // Open compact: Qt's default dock height lands the timeline tall on every
    // first launch (the user has to shrink it each time). Pin an explicit
    // initial height so the media space above keeps most of the window. (There
    // is no saveState/restoreState yet, so this is the standing default.)
    mw.resizeDocks({timeline_dock}, {265}, Qt::Vertical);

    // Central workspace = the viewer column (regions lock into place around it).
    if (QWidget* vf = mw.ui->viewerFrame) {
        auto* vf_layout = new QVBoxLayout(vf);
        vf_layout->setContentsMargins(0, 0, 0, 0);
        vf_layout->setSpacing(0);
        vf_layout->addWidget(viewer_column);
    }

    // ---- 7b. DELIVER page docks — settings left, render queue right ----
    mw.deliver_settings_ = new DeliverSettingsPanel(&mw);
    mw.deliver_settings_->setObjectName(QStringLiteral("deliverSettings"));
    mw.deliver_settings_dock_ = new QDockWidget(MainWindow::tr("Deliver Settings"), &mw);
    mw.deliver_settings_dock_->setObjectName(QStringLiteral("deliverSettingsDock"));
    mw.deliver_settings_dock_->setWidget(mw.deliver_settings_);
    mw.deliver_settings_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    mw.deliver_settings_dock_->setMinimumWidth(360);
    mw.addDockWidget(Qt::LeftDockWidgetArea, mw.deliver_settings_dock_);
    mw.deliver_settings_dock_->hide();

    mw.deliver_queue_panel_ = new RenderQueuePanel(&mw);
    mw.deliver_queue_panel_->setObjectName(QStringLiteral("deliverQueue"));
    mw.deliver_queue_panel_->set_queue(&mw.render_queue_);
    mw.deliver_queue_dock_ = new QDockWidget(MainWindow::tr("Render Queue"), &mw);
    mw.deliver_queue_dock_->setObjectName(QStringLiteral("deliverQueueDock"));
    mw.deliver_queue_dock_->setWidget(mw.deliver_queue_panel_);
    mw.deliver_queue_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    mw.deliver_queue_dock_->setMinimumWidth(300);
    mw.addDockWidget(Qt::RightDockWidgetArea, mw.deliver_queue_dock_);
    mw.deliver_queue_dock_->hide();

    // Deliver widget signals -> MainWindow actions.
    QObject::connect(mw.deliver_settings_, &DeliverSettingsPanel::add_to_queue_clicked, &mw,
            &MainWindow::add_current_to_render_queue);
    QObject::connect(mw.deliver_settings_, &DeliverSettingsPanel::render_all_clicked, &mw,
            &MainWindow::render_all_from_queue);
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::render_all_clicked, &mw,
            &MainWindow::render_all_from_queue);
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::clear_queued_clicked, &mw,
            [&mw] { mw.render_queue_.clear_queued(); mw.has_unsaved_changes_ = true;
                    mw.reflect_render_queue(); });
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::job_remove_clicked, &mw,
            [&mw](uint64_t id) { mw.render_queue_.remove(id); mw.has_unsaved_changes_ = true;
                                 mw.reflect_render_queue(); });
    QObject::connect(mw.deliver_queue_panel_, &RenderQueuePanel::cancel_all_clicked, &mw,
            [&mw] { mw.render_queue_.cancel_all(); mw.has_unsaved_changes_ = true;
                    mw.reflect_render_queue(); });

    // Reflect queue changes into the UI panel (called on the main thread via a
    // queued-style refresh using QMetaObject to stay thread-safe with the
    // worker thread).
    mw.render_queue_.on_changed = [&mw] {
        QMetaObject::invokeMethod(&mw, [&mw] { mw.reflect_render_queue(); },
                                  Qt::QueuedConnection);
    };

    // Pop a dialog when a render job fails, so the user isn't left guessing at
    // a bare "Failed" status. Errors are categorized so each kind of failure
    // gets its own popup type instead of a single generic message:
    //   - Configuration problems (missing output path / no codec selected) are
    //     shown as a persistent Information prompt to fix settings and retry.
    //   - Encoder/container/open-file failures are shown as a Warning with the
    //     FFmpeg detail.
    //   - Anything unexpected is shown as a Critical error.
    mw.render_queue_.on_job_finished = [&mw](uint64_t id) {
        QMetaObject::invokeMethod(&mw, [&mw, id] {
            for (const auto& j : mw.render_queue_.jobs()) {
                if (j.id != id) continue;
                if (j.status != canvas::core::RenderJob::Status::Failed) break;
                const QString detail = j.error.empty()
                    ? MainWindow::tr("The renderer reported no error message. Check canvas_debug.log.")
                    : QString::fromStdString(j.error);
                const QString title = MainWindow::tr("Render Failed");
                if (j.error.find("Output path") != std::string::npos) {
                    QMessageBox::information(
                        &mw, title,
                        MainWindow::tr("There's no output location for \"%1\".\n\n%2\n\n"
                                       "Choose an output folder on the Deliver panel, then add to the queue again.")
                            .arg(QString::fromStdString(j.name), detail));
                } else if (j.error.find("codec") != std::string::npos ||
                           j.error.find("encoder") != std::string::npos) {
                    QMessageBox::warning(
                        &mw, title,
                        MainWindow::tr("\"%1\" couldn't start encoding.\n\n%2\n\n"
                                       "Try a different codec or encoder in Deliver settings.")
                            .arg(QString::fromStdString(j.name), detail));
                } else {
                    QMessageBox::critical(
                        &mw, title,
                        MainWindow::tr("\"%1\" failed.\n\n%2")
                            .arg(QString::fromStdString(j.name), detail));
                }
                break;
            }
        }, Qt::QueuedConnection);
    };

    mw.status_ = mw.ui->statusbar;
    mw.status_->setStyleSheet(QStringLiteral("background-color: #11131A; color: #9AA0B0;"));
    mw.status_->addPermanentWidget(new QLabel(MainWindow::tr("Nova Canvas Studio"), mw.status_));

    mw.connect_timeline();

    // The inspector's Transform/Composite categories subscribe to selection
    // changes here (timeline_ exists only after build_center_workspace); the
    // Audio/Transition/File pages attach the same way.
    attach_inspector_visual(mw, mw.timeline_);
    attach_inspector_audio(mw, mw.timeline_);
    attach_inspector_transition(mw, mw.timeline_);
    attach_inspector_file(mw, mw.timeline_);
}

}  // namespace canvas::gui