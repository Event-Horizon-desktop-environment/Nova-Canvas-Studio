// Top status strip: format/edited/fps readouts, the big timecode, and the
// Quick Export / Full Screen / Mixer / Metadata / Inspector cluster. The
// page-switcher toolbar and the playback transport bar moved to their own
// files (ShellPageBar.cpp / ShellTransportBar.cpp, splitplan refactor).

#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

#include "core/timecode.hpp"

#include <QAction>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

namespace canvas::gui {

QWidget* build_top_bar(MainWindow& mw) {
    auto* top_bar = new QWidget(&mw);
    top_bar->setObjectName(QStringLiteral("topStatusBar"));
    apply_theme_style(top_bar, &top_status_bar_style);
    auto* top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(12, 6, 12, 6);
    top_bar_layout->setSpacing(12);

    auto* format_label = new QLabel(MainWindow::tr("1440p"), top_bar);
    apply_theme_style(format_label, [] { return QStringLiteral("color: %1; font-size: 12px;").arg(css(tokens().ink_muted)); });
    auto* edited_label = new QLabel(QStringLiteral("\u00B7 ") + MainWindow::tr("Edited"), top_bar);
    apply_theme_style(edited_label, [] { return QStringLiteral("color: %1; font-size: 12px;").arg(css(tokens().ink_faint)); });
    mw.fps_label_ = new QLabel(MainWindow::tr("0 fps"), top_bar);
    apply_theme_style(mw.fps_label_, [] { return QStringLiteral("color: %1; font-size: 12px;").arg(css(tokens().ink_faint)); });
    top_bar_layout->addWidget(format_label);
    top_bar_layout->addWidget(edited_label);
    auto* fps_sep = new QLabel(QStringLiteral("\u00B7"), top_bar);
    apply_theme_style(fps_sep, [] { return QStringLiteral("color: %1; font-size: 12px;").arg(css(tokens().ink_faint)); });
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

}  // namespace canvas::gui