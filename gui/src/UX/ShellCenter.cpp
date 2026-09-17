#include "UX/MainWindow.hpp"
#include "UX/InspectorAudio.hpp"
#include "UX/InspectorFile.hpp"
#include "UX/InspectorSubtitles.hpp"
#include "UX/InspectorTransition.hpp"
#include "UX/InspectorVisual.hpp"
#include "ui_MainWindow.h"

#include "Widgets/viewer_gl.hpp"

#include "features/timeline/view_options_menu.hpp"

#include <QAction>
#include <QDebug>
#include <QDockWidget>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSplitter>
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
#include "features/source_preview/source_viewer_panel.hpp"

namespace canvas::gui {

namespace {

QAction* add_overlay_action(MainWindow& mw, ViewerGL* viewer, QMenu& m, const char* key,
                            ViewerGL::Overlay overlay, const QString& label) {
    QAction* a = m.addAction(label);
    a->setCheckable(true);
    a->setChecked(viewer->overlay_enabled(overlay));
    QObject::connect(a, &QAction::toggled, &mw, [&mw, viewer, key, overlay](bool on) {
        viewer->set_overlay(overlay, on);
        QSettings().setValue(QLatin1String(key), on);
        if (auto* guides = mw.findChild<QToolButton*>(QStringLiteral("guidesButton"))) {
            const QSignalBlocker b(guides);
            guides->setChecked(viewer->overlay_enabled(ViewerGL::Overlay::SafeAreas) ||
                               viewer->overlay_enabled(ViewerGL::Overlay::ThirdsGrid));
        }
    });
    return a;
}

}

void build_center_workspace(MainWindow& mw) {
    mw.viewer_ = new ViewerGL(&mw);

    mw.source_panel_ = new source_preview::SourceViewerPanel(&mw);
    mw.source_panel_->viewer()->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    const bool dual_view_on = QSettings().value(QStringLiteral("dualViewer"), false).toBool();
    if (dual_view_on)
        mw.source_panel_->setVisible(true);
    QObject::connect(mw.source_panel_, &source_preview::SourceViewerPanel::play_clicked, &mw,
            [&mw] { mw.src_preview_.toggle_play_pause(); });
    QObject::connect(mw.source_panel_, &source_preview::SourceViewerPanel::scrub_fraction, &mw,
            [&mw](double fraction) {
                if (mw.src_preview_.is_playing()) mw.src_preview_.pause();
                mw.src_preview_.scrub_fraction(fraction);
            });

    mw.viewer_->setContextMenuPolicy(Qt::CustomContextMenu);
    const bool saved_scale = QSettings().value(QStringLiteral("viewerScaleFill"), false).toBool();
    mw.viewer_->set_scale_mode(saved_scale ? ViewerGL::ScaleMode::Fill : ViewerGL::ScaleMode::Fit);
    {
        QSettings settings;
        mw.viewer_->set_overlay(ViewerGL::Overlay::SafeAreas,
                                settings.value(QStringLiteral("viewerOverlaySafeAreas"), false).toBool());
        mw.viewer_->set_overlay(ViewerGL::Overlay::ThirdsGrid,
                                settings.value(QStringLiteral("viewerOverlayThirdsGrid"), false).toBool());
        mw.viewer_->set_overlay(ViewerGL::Overlay::PlaybackBadge,
                                settings.value(QStringLiteral("viewerOverlayPlaybackBadge"), false).toBool());
    }
    QObject::connect(mw.viewer_, &QWidget::customContextMenuRequested, &mw,
            [&mw](const QPoint& pos) {
                QMenu menu(MainWindow::tr("Monitor Scale"), &mw);
                apply_rounded_menu(&menu);
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

                menu.addSeparator();
                add_overlay_action(mw, mw.viewer_, menu, "viewerOverlaySafeAreas", ViewerGL::Overlay::SafeAreas,
                                  MainWindow::tr("Show Safe Areas"));
                add_overlay_action(mw, mw.viewer_, menu, "viewerOverlayThirdsGrid", ViewerGL::Overlay::ThirdsGrid,
                                  MainWindow::tr("Show Thirds Grid"));
                add_overlay_action(mw, mw.viewer_, menu, "viewerOverlayPlaybackBadge", ViewerGL::Overlay::PlaybackBadge,
                                  MainWindow::tr("Show Playback Indicator"));

                menu.exec(mw.viewer_->mapToGlobal(pos));
            });

    auto* contextual_bar = new QToolBar(MainWindow::tr("Editing Tools"), &mw);
    contextual_bar->setMovable(false);
    contextual_bar->setObjectName(QStringLiteral("contextualTools"));
    apply_theme_style(contextual_bar, &timeline_tools_style);
    contextual_bar->setIconSize(QSize(16, 16));

    auto make_tool = [&mw](QToolBar* bar, const char* icon_name, const char* tip,
                           bool checkable) -> QToolButton* {
        auto* b = new QToolButton(bar);
        b->setIcon(icon(icon_name));
        b->setIconSize(QSize(16, 16));
        b->setToolTip(MainWindow::tr(tip));
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        apply_theme_style(b, &flat_tool_style);
        bar->addWidget(b);
        return b;
    };

    make_tool(contextual_bar, "menu", "Application menu", false);
    auto* snap_toggle = make_tool(contextual_bar, "snap", "Snap (toggle)", true);
    snap_toggle->setChecked(true);
    make_tool(contextual_bar, "mic", "Record/take mic", false);

    contextual_bar->addSeparator();

    auto* tool_select = new QToolButton(contextual_bar);
    tool_select->setIcon(icon("select"));
    tool_select->setIconSize(QSize(16, 16));
    tool_select->setToolTip(MainWindow::tr("Selection (A)"));
    tool_select->setCheckable(true);
    tool_select->setChecked(true);
    tool_select->setAutoRaise(true);
    apply_theme_style(tool_select, &flat_tool_style);
    contextual_bar->addWidget(tool_select);

    auto* tool_trim = new QToolButton(contextual_bar);
    tool_trim->setIcon(icon("trim"));
    tool_trim->setIconSize(QSize(16, 16));
    tool_trim->setToolTip(MainWindow::tr("Trim (T)"));
    tool_trim->setCheckable(true);
    tool_trim->setAutoRaise(true);
    apply_theme_style(tool_trim, &tool_cluster_style);
    contextual_bar->addWidget(tool_trim);
    auto* tool_blade = new QToolButton(contextual_bar);
    tool_blade->setIcon(icon("razor_blade"));
    tool_blade->setIconSize(QSize(16, 16));
    tool_blade->setToolTip(MainWindow::tr("Blade (B)"));
    tool_blade->setCheckable(true);
    tool_blade->setAutoRaise(true);
    apply_theme_style(tool_blade, &tool_cluster_style);
    contextual_bar->addWidget(tool_blade);
    auto* tool_mode = new QToolButton(contextual_bar);
    tool_mode->setIcon(icon("mode"));
    tool_mode->setIconSize(QSize(16, 16));
    tool_mode->setToolTip(MainWindow::tr("Edit mode"));
    tool_mode->setCheckable(true);
    tool_mode->setAutoRaise(true);
    apply_theme_style(tool_mode, &tool_cluster_style);
    contextual_bar->addWidget(tool_mode);

    contextual_bar->addSeparator();
    auto* linked_sel = new QToolButton(contextual_bar);
    linked_sel->setIcon(icon("linked_sel"));
    linked_sel->setIconSize(QSize(16, 16));
    linked_sel->setCheckable(true);
    linked_sel->setChecked(true);
    linked_sel->setToolTip(MainWindow::tr("Linked selection"));
    linked_sel->setAutoRaise(true);
    apply_theme_style(linked_sel, &flat_tool_style);
    contextual_bar->addWidget(linked_sel);
    auto* sync_lock = new QToolButton(contextual_bar);
    sync_lock->setIcon(icon("sync_lock"));
    sync_lock->setIconSize(QSize(16, 16));
    sync_lock->setCheckable(true);
    sync_lock->setToolTip(MainWindow::tr("Track lock"));
    sync_lock->setAutoRaise(true);
    apply_theme_style(sync_lock, &flat_tool_style);
    contextual_bar->addWidget(sync_lock);

    contextual_bar->addSeparator();

    make_tool(contextual_bar, "color_tag", "Track color", false);
    auto* marker_color = new QToolButton(contextual_bar);
    marker_color->setIcon(icon("mark_in", QColor(0xC9, 0x86, 0x3A)));
    marker_color->setIconSize(QSize(14, 14));
    marker_color->setToolTip(MainWindow::tr("Marker color"));
    marker_color->setAutoRaise(true);
    apply_theme_style(marker_color, &flat_tool_style);
    contextual_bar->addWidget(marker_color);
    auto* marker_down = new QToolButton(contextual_bar);
    marker_down->setIcon(icon("chevron_down", QColor(0xC9, 0x86, 0x3A)));
    marker_down->setToolTip(MainWindow::tr("Marker color"));
    marker_down->setAutoRaise(true);
    apply_theme_style(marker_down, &flat_tool_style);
    contextual_bar->addWidget(marker_down);

    auto* bar_spacer = new QWidget(contextual_bar);
    bar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    contextual_bar->addWidget(bar_spacer);

    auto* zoom_fit = new QToolButton(contextual_bar);
    zoom_fit->setIcon(icon("fit"));
    zoom_fit->setIconSize(QSize(16, 16));
    zoom_fit->setToolTip(MainWindow::tr("Zoom to fit"));
    zoom_fit->setAutoRaise(true);
    apply_theme_style(zoom_fit, &flat_tool_style);
    contextual_bar->addWidget(zoom_fit);
    auto* zoom_out = new QToolButton(contextual_bar);
    zoom_out->setIcon(icon("zoom_out"));
    zoom_out->setIconSize(QSize(16, 16));
    zoom_out->setAutoRaise(true);
    apply_theme_style(zoom_out, &flat_tool_style);
    contextual_bar->addWidget(zoom_out);
    auto* zoom_slider = new QSlider(Qt::Horizontal, contextual_bar);
    zoom_slider->setRange(0, 100);
    zoom_slider->setValue(4);
    zoom_slider->setFixedWidth(140);
    apply_theme_style(zoom_slider, &slider_style);
    contextual_bar->addWidget(zoom_slider);
    auto* zoom_in = new QToolButton(contextual_bar);
    zoom_in->setIcon(icon("zoom_in"));
    zoom_in->setIconSize(QSize(16, 16));
    zoom_in->setAutoRaise(true);
    apply_theme_style(zoom_in, &flat_tool_style);
    contextual_bar->addWidget(zoom_in);

    auto* volume_icon = new QToolButton(contextual_bar);
    volume_icon->setIcon(icon("volume"));
    volume_icon->setIconSize(QSize(16, 16));
    volume_icon->setToolTip(MainWindow::tr("Monitoring volume"));
    volume_icon->setAutoRaise(true);
    apply_theme_style(volume_icon, &flat_tool_style);
    contextual_bar->addWidget(volume_icon);
    auto* volume_slider = new QSlider(Qt::Horizontal, contextual_bar);
    volume_slider->setRange(0, 100);
    volume_slider->setValue(80);
    volume_slider->setFixedWidth(90);
    apply_theme_style(volume_slider, &slider_style);
    contextual_bar->addWidget(volume_slider);

    auto* dim_btn = new QToolButton(contextual_bar);
    dim_btn->setText(MainWindow::tr("DIM"));
    dim_btn->setToolTip(MainWindow::tr("Temporarily dip monitoring volume"));
    dim_btn->setAutoRaise(true);
    dim_btn->setCheckable(true);
    apply_theme_style(dim_btn, &outline_pill_style);
    contextual_bar->addWidget(dim_btn);

    const auto sync_zoom_slider = [&mw, zoom_slider, zoom_in, zoom_out] {
        if (!mw.timeline_) return;
        const double min_pct = mw.timeline_->interactive_floor_percent();
        const double max_pct = TimelineWidget::kZoomMaxPercent;
        const double pct = std::clamp(mw.timeline_->zoom_percent(), min_pct, max_pct);
        const int v = static_cast<int>(std::round(
            (pct - min_pct) / (max_pct - min_pct) * 100.0));
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
        if (!mw.timeline_) return;
        const double min_pct = mw.timeline_->interactive_floor_percent();
        const double max_pct = TimelineWidget::kZoomMaxPercent;
        const double pct = min_pct + (max_pct - min_pct) * v / 100.0;
        mw.timeline_->set_zoom_percent(pct);
    });

    sync_zoom_slider();

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

    auto* viewer_frame = new QFrame(&mw);
    viewer_frame->setObjectName(QStringLiteral("viewerFrame"));
    apply_theme_style(viewer_frame, &viewer_frame_style);
    auto* viewer_frame_layout = new QVBoxLayout(viewer_frame);
    viewer_frame_layout->setContentsMargins(8, 8, 8, 8);
    auto* monitor_split = new QSplitter(Qt::Horizontal, viewer_frame);
    monitor_split->setObjectName(QStringLiteral("monitorSplit"));
    monitor_split->setChildrenCollapsible(false);
    monitor_split->setHandleWidth(4);
    monitor_split->addWidget(mw.source_panel_);
    monitor_split->addWidget(mw.viewer_);
    monitor_split->setStretchFactor(0, 1);
    monitor_split->setStretchFactor(1, 1);
    if (dual_view_on) monitor_split->setSizes({1, 1});
    qWarning() << "[srcprv] dual-view startup source_panel_visible=" << dual_view_on;
    viewer_frame_layout->addWidget(monitor_split, 1);

    auto* transport = build_transport_bar(mw);

    auto* top_bar = build_top_bar(mw);

    auto* viewer_column = new QWidget(&mw);
    viewer_column->setObjectName(QStringLiteral("viewerColumn"));
    apply_theme_style(viewer_column, [] {
        return QStringLiteral("QWidget#viewerColumn { background-color: %1; }")
            .arg(css(tokens().surface));
    });
    auto* viewer_layout = new QVBoxLayout(viewer_column);
    viewer_layout->setContentsMargins(0, 0, 0, 0);
    viewer_layout->setSpacing(0);

    auto* top_scrub = new QWidget(viewer_column);
    top_scrub->setObjectName(QStringLiteral("topScrubBar"));
    apply_theme_style(top_scrub, [] {
        return QStringLiteral("QWidget#topScrubBar { background: %1; border-bottom: 1px solid %2; }")
            .arg(css(tokens().surface), css(tokens().border));
    });
    auto* top_scrub_layout = new QHBoxLayout(top_scrub);
    top_scrub_layout->setContentsMargins(8, 4, 8, 4);
    top_scrub_layout->setSpacing(8);
    auto* dual_view = new QToolButton(&mw);
    dual_view->setIcon(icon("Dual-View"));
    dual_view->setIconSize(QSize(14, 14));
    dual_view->setCheckable(true);
    dual_view->setChecked(dual_view_on);
    dual_view->setAutoRaise(true);
    dual_view->setToolTip(MainWindow::tr("Dual Viewer — split the monitor with a scrubbable source preview"));
    apply_theme_style(dual_view, &flat_tool_style);
    QObject::connect(dual_view, &QToolButton::toggled, &mw,
            [&mw, dual_view, monitor_split](bool on) {
                mw.source_panel_->setVisible(on);
                QSettings().setValue(QStringLiteral("dualViewer"), on);
                qWarning() << "[srcprv] dual-view toggle on=" << on
                           << " (persisted)";
                if (on) {
                    monitor_split->setSizes({1, 1});
                } else {
                    mw.src_preview_.end_hover_scrub();
                    mw.src_preview_.release_audio();
                    mw.source_hovering_ = false;
                    monitor_split->setSizes({0, 1});
                }
                dual_view->setChecked(on);
            });
    auto* guides_btn = new QToolButton(&mw);
    guides_btn->setObjectName(QStringLiteral("guidesButton"));
    guides_btn->setIcon(icon("grid"));
    guides_btn->setIconSize(QSize(14, 14));
    guides_btn->setCheckable(true);
    guides_btn->setChecked(mw.viewer_->overlay_enabled(ViewerGL::Overlay::SafeAreas) ||
                           mw.viewer_->overlay_enabled(ViewerGL::Overlay::ThirdsGrid));
    guides_btn->setAutoRaise(true);
    guides_btn->setToolTip(MainWindow::tr("Monitor Overlays — toggle guides, or pick each overlay from the menu"));
    apply_theme_style(guides_btn, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QToolButton#guidesButton { background: transparent; border: 1px solid transparent;"
            " border-radius: 8px; padding: 5px; }"
            "QToolButton#guidesButton:hover { background: %1; }"
            "QToolButton#guidesButton:pressed { background: %2; }"
            "QToolButton#guidesButton:checked { background: %1; border: 1px solid %3; }")
            .arg(css(t.state_hover), css(t.state_press), css(t.border));
    });
    auto* guides_sep = new QWidget(&mw);
    guides_sep->setObjectName(QStringLiteral("guidesSeparator"));
    guides_sep->setFixedSize(1, 16);
    apply_theme_style(guides_sep, [] {
        return QStringLiteral("QWidget#guidesSeparator { background-color: %1; }")
                .arg(css(tokens().border));
    });
    auto* guides_menu_btn = new QToolButton(&mw);
    guides_menu_btn->setObjectName(QStringLiteral("guidesMenuButton"));
    guides_menu_btn->setIcon(icon("chevron_down"));
    guides_menu_btn->setIconSize(QSize(12, 12));
    guides_menu_btn->setAutoRaise(true);
    guides_menu_btn->setPopupMode(QToolButton::InstantPopup);
    guides_menu_btn->setToolTip(MainWindow::tr("Overlay options"));
    apply_theme_style(guides_menu_btn, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QToolButton#guidesMenuButton { background: transparent; border: none;"
            " border-radius: 8px; padding: 4px; }"
            "QToolButton#guidesMenuButton:hover { background: %1; }"
            "QToolButton#guidesMenuButton:pressed { background: %2; }"
            "QToolButton#guidesMenuButton::menu-indicator { image: none;"
            " width: 0px; height: 0px; }")
            .arg(css(t.state_hover), css(t.state_press));
    });
    auto* guides_cluster = new QWidget(&mw);
    auto* cluster_row = new QHBoxLayout(guides_cluster);
    cluster_row->setContentsMargins(0, 0, 0, 0);
    cluster_row->setSpacing(3);
    cluster_row->addWidget(guides_btn);
    cluster_row->addWidget(guides_sep);
    cluster_row->addWidget(guides_menu_btn);
    cluster_row->setAlignment(guides_sep, Qt::AlignCenter);
    auto* guides_menu = new QMenu(guides_menu_btn);
    apply_rounded_menu(guides_menu);
    QAction* guides_safe = add_overlay_action(mw, mw.viewer_, *guides_menu, "viewerOverlaySafeAreas",
                                              ViewerGL::Overlay::SafeAreas,
                                              MainWindow::tr("Show Safe Areas"));
    QAction* guides_thirds = add_overlay_action(mw, mw.viewer_, *guides_menu, "viewerOverlayThirdsGrid",
                                                ViewerGL::Overlay::ThirdsGrid,
                                                MainWindow::tr("Show Thirds Grid"));
    QAction* guides_badge = add_overlay_action(mw, mw.viewer_, *guides_menu, "viewerOverlayPlaybackBadge",
                                               ViewerGL::Overlay::PlaybackBadge,
                                               MainWindow::tr("Show Playback Indicator"));
    guides_menu_btn->setMenu(guides_menu);
    QObject::connect(guides_btn, &QToolButton::toggled, &mw,
            [&mw, guides_safe, guides_thirds](bool on) {
                mw.viewer_->set_overlay(ViewerGL::Overlay::SafeAreas, on);
                mw.viewer_->set_overlay(ViewerGL::Overlay::ThirdsGrid, on);
                QSettings().setValue(QStringLiteral("viewerOverlaySafeAreas"), on);
                QSettings().setValue(QStringLiteral("viewerOverlayThirdsGrid"), on);
                const QSignalBlocker b1(guides_safe);
                const QSignalBlocker b2(guides_thirds);
                guides_safe->setChecked(on);
                guides_thirds->setChecked(on);
            });
    QObject::connect(guides_menu, &QMenu::aboutToShow, &mw,
            [&mw, guides_btn, guides_safe, guides_thirds, guides_badge] {
                const QSignalBlocker b1(guides_safe);
                const QSignalBlocker b2(guides_thirds);
                const QSignalBlocker b3(guides_badge);
                const QSignalBlocker b4(guides_btn);
                guides_safe->setChecked(mw.viewer_->overlay_enabled(ViewerGL::Overlay::SafeAreas));
                guides_thirds->setChecked(mw.viewer_->overlay_enabled(ViewerGL::Overlay::ThirdsGrid));
                guides_badge->setChecked(mw.viewer_->overlay_enabled(ViewerGL::Overlay::PlaybackBadge));
                guides_btn->setChecked(mw.viewer_->overlay_enabled(ViewerGL::Overlay::SafeAreas) ||
                                       mw.viewer_->overlay_enabled(ViewerGL::Overlay::ThirdsGrid));
            });
    auto* media_collapse_btn = new QToolButton(&mw);
    media_collapse_btn->setObjectName(QStringLiteral("mediaCollapseBtn"));
    media_collapse_btn->setIcon(icon("Collapse"));
    media_collapse_btn->setIconSize(QSize(16, 16));
    media_collapse_btn->setAutoRaise(true);
    media_collapse_btn->setFixedSize(30, 30);
    media_collapse_btn->setCursor(Qt::PointingHandCursor);
    media_collapse_btn->setToolTip(MainWindow::tr("Collapse Media Pool"));
    apply_theme_style(media_collapse_btn, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QToolButton#mediaCollapseBtn { background: transparent; border: none;"
            " border-radius: 6px; padding: 4px; }"
            "QToolButton#mediaCollapseBtn:hover { background-color: %1; }"
            "QToolButton#mediaCollapseBtn:pressed { background-color: %2; }")
            .arg(css(t.state_hover), css(t.border));
    });
    QObject::connect(media_collapse_btn, &QToolButton::clicked, &mw,
            [media_collapse_btn, &mw] {
        const bool collapsing =
            mw.corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea;
        if (collapsing)
            mw.setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
        else
            mw.setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
        mw.resizeDocks({mw.media_dock_}, {mw.media_dock_->width()}, Qt::Horizontal);
        const QSize cur = mw.size();
        mw.resize(cur.width() + 1, cur.height());
        mw.resize(cur);
        media_collapse_btn->setIcon(icon(collapsing ? "Expand" : "Collapse"));
        media_collapse_btn->setToolTip(MainWindow::tr(collapsing ? "Expand Media Pool"
                                                                 : "Collapse Media Pool"));
    });
    if (auto* tl = qobject_cast<QHBoxLayout*>(top_bar->layout())) {
        tl->insertWidget(0, media_collapse_btn);
        tl->insertWidget(1, dual_view);
        tl->insertWidget(2, guides_cluster);
    }
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
    apply_theme_style(scrub_lock, &flat_tool_style);
    scrub_lock->setToolTip(MainWindow::tr("Lock timeline"));
    top_scrub_layout->addWidget(scrub_lock);
    viewer_layout->addWidget(top_bar);
    auto* viewer_inner = new QWidget(viewer_column);
    auto* viewer_inner_layout = new QVBoxLayout(viewer_inner);
    viewer_inner_layout->setContentsMargins(8, 8, 8, 8);
    viewer_inner_layout->setSpacing(4);
    viewer_inner_layout->addWidget(viewer_frame, 1);
    viewer_layout->addWidget(viewer_inner, 1);

    mw.timeline_ = new TimelineWidget(&mw);
    mw.timeline_->set_sequence(&mw.project_->sequence);
    mw.timeline_->set_fps(mw.project_->sequence.fps);
    mw.timeline_->set_thumbnail_service(&mw.thumbnails_);
    apply_view_options(mw);
    mw.thumbnails_.set_cache_dir(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("thumbs")));
    QObject::connect(mw.timeline_, &TimelineWidget::media_dropped, &mw,
            [&mw](int media_id, int64_t frame, double scene_y) { mw.place_media_at(media_id, frame, canvas::core::Placement::Overwrite, scene_y); });
    QObject::connect(mw.timeline_, &TimelineWidget::title_dropped, &mw,
            [&mw](const QString& preset, int64_t frame, double) { mw.place_title_at(preset, frame); });
    QObject::connect(mw.timeline_, &TimelineWidget::transition_dropped, &mw,
            [&mw](const QString& id, int64_t frame, double scene_y) { mw.apply_transition_from_toolbox(id, frame, scene_y); });

    sync_zoom_slider();

    auto* timeline_frame = new QFrame(&mw);
    timeline_frame->setObjectName(QStringLiteral("timelineFrame"));
    apply_theme_style(timeline_frame, &timeline_frame_style);
    constexpr int kTimelineBarRows = 124;
    auto* timeline_frame_layout = new QVBoxLayout(timeline_frame);
    timeline_frame_layout->setContentsMargins(8, 8, 8, 8);
    timeline_frame_layout->setSpacing(4);
    timeline_frame_layout->addWidget(contextual_bar);
    timeline_frame_layout->addWidget(transport);
    timeline_frame_layout->addWidget(top_scrub);
    timeline_frame_layout->addWidget(mw.timeline_, 1);

    auto* timeline_dock = mw.ui->timelineDock;
    timeline_dock->setObjectName(QStringLiteral("timelineDock"));
    apply_theme_style(timeline_dock, &dock_glow_style);
    auto* timeline_title = new QWidget(timeline_dock);
    timeline_title->setObjectName(QStringLiteral("timelineDockTitle"));
    apply_theme_style(timeline_title, [] {
        return QStringLiteral("QWidget#timelineDockTitle { background: transparent;"
                              " border: none; }");
    });
    timeline_dock->setTitleBarWidget(timeline_title);
    timeline_dock->setWidget(timeline_frame);
    timeline_dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    timeline_dock->setMinimumHeight(140 + kTimelineBarRows);
    QObject::connect(mw.timeline_, &TimelineWidget::content_height_changed, &mw,
            [&mw, timeline_dock](int height_px) {
                const int cap = std::max(265, static_cast<int>(mw.height() * 3 / 4));
                mw.resizeDocks({timeline_dock},
                        {std::min(height_px, cap) + kTimelineBarRows}, Qt::Vertical);
            });
    const int initial_height = mw.timeline_->desired_timeline_height();
    mw.resizeDocks({timeline_dock},
            {std::max(initial_height, 265) + kTimelineBarRows}, Qt::Vertical);

    if (QWidget* vf = mw.ui->viewerFrame) {
        auto* vf_layout = new QVBoxLayout(vf);
        vf_layout->setContentsMargins(0, 0, 0, 0);
        vf_layout->setSpacing(0);
        vf_layout->addWidget(viewer_column);
    }

    build_deliver_docks(mw);

    mw.status_ = mw.ui->statusbar;
    const ThemeTokens& t = tokens();
    mw.status_->setStyleSheet(QStringLiteral("background-color: %1; color: %2; border-top: 1px solid %3;")
                                  .arg(css(t.surface), css(t.ink_muted), css(t.border_soft)));

    mw.connect_timeline();

    attach_inspector_visual(mw, mw.timeline_);
    attach_inspector_audio(mw, mw.timeline_);
    attach_inspector_transition(mw, mw.timeline_);
    attach_inspector_file(mw, mw.timeline_);
    attach_inspector_subtitles(mw, mw.timeline_);

    build_color_page(mw);
}

}
