#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "ui_MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QSettings>

#include <functional>

namespace canvas::gui {

void build_app_menus(MainWindow& mw) {
    // 1. MENU BAR — the full editor-standard row. A leading "Nova Canvas" app
    //    menu carries app-level commands (About / Appearance / Preferences /
    //    Quit, per HIG's app-menu anatomy), then the workflow menus in the
    //    Resolve order: File/Edit/Trim/Timeline/Clip/Mark/View/Playback/
    //    Fusion/Color/Fairlight/Workspace/Help. Only File/Edit/Playback/View/
    //    Timeline/Mark are wired for v1; the rest are present with stub items
    //    so the chrome matches the reference.
    auto* canvas_menu = mw.ui->menubar->addMenu(MainWindow::tr("Nova Canvas"));
    canvas_menu->addAction(MainWindow::tr("&About Nova Canvas Studio"), &mw, [&mw] {
        QMessageBox::about(
            &mw, MainWindow::tr("About Nova Canvas Studio"),
            MainWindow::tr("Nova Canvas Studio\n\n"
                           "C++20 / Qt %1 / FFmpeg nonlinear video editor — "
                           "dark, editor-grade UI.\n\n"
                           "Ships an auto-detected \u201cHyprDark\u201d palette that "
                           "pre-compensates for Hyprland's native-Wayland "
                           "colour-management pass.")
                .arg(QString::fromUtf8(qVersion())));
    });
    canvas_menu->addSeparator();

    // Appearance: the three theme states as exclusive choices. On Hyprland's
    // Wayland backend "Dark (Hyprland)" is the auto default so the palette
    // lands as designed through the compositor's FP16 re-quantization; plain
    // "Dark" exists for people who want the uncompensated set. Choices are
    // remembered (appearance/hypr_dark) and override the auto rule next launch.
    auto* appearance = canvas_menu->addMenu(MainWindow::tr("A&ppearance"));
    apply_rounded_menu(appearance);
    auto* appearance_group = new QActionGroup(appearance);
    appearance_group->setExclusive(true);
    const bool light_now = is_light();
    const bool hypr_now = is_hypr_dark();
    auto* dark_action = appearance->addAction(MainWindow::tr("&Dark"));
    dark_action->setCheckable(true);
    dark_action->setChecked(!light_now && !hypr_now);
    auto* hypr_action = appearance->addAction(MainWindow::tr("Dark (&Hyprland)"));
    hypr_action->setCheckable(true);
    hypr_action->setChecked(!light_now && hypr_now);
    hypr_action->setToolTip(MainWindow::tr(
        "Compensated for Hyprland's native-Wayland colour-management pass."));
    auto* light_action = appearance->addAction(MainWindow::tr("&Light"));
    light_action->setCheckable(true);
    light_action->setChecked(light_now);
    appearance_group->addAction(dark_action);
    appearance_group->addAction(hypr_action);
    appearance_group->addAction(light_action);
    const auto select_theme = [](bool light, bool hypr) {
        set_light(light);
        set_hypr_dark(hypr);
        QSettings settings;
        settings.setValue(QStringLiteral("appearance/theme"),
                          light ? QStringLiteral("light") : QStringLiteral("dark"));
        settings.setValue(QStringLiteral("appearance/hypr_dark"), hypr);
    };
    QObject::connect(dark_action, &QAction::triggered, &mw,
                     [select_theme] { select_theme(false, false); });
    QObject::connect(hypr_action, &QAction::triggered, &mw,
                     [select_theme] { select_theme(false, true); });
    QObject::connect(light_action, &QAction::triggered, &mw,
                     [select_theme] { select_theme(true, false); });

    // App-level settings belong in the app menu, not the Edit menu (HIG: the
    // app menu lists items that apply to the app as a whole).
    const auto show_preferences = [&mw]() {
        // Audible-scrubbing preference. A small popup menu keeps the option
        // discoverable without a dedicated settings dialog.
        QMenu menu;
        apply_rounded_menu(&menu);
        const bool saved = QSettings().value(QStringLiteral("scrubAudioEnabled"), true).toBool();
        mw.controller_.set_scrub_audio_enabled(saved);
        auto* scrub_audio = menu.addAction(MainWindow::tr("Audible Scrubbing"));
        scrub_audio->setCheckable(true);
        scrub_audio->setChecked(saved);
        QObject::connect(scrub_audio, &QAction::toggled, &mw, [&mw](bool on) {
            mw.controller_.set_scrub_audio_enabled(on);
            QSettings().setValue(QStringLiteral("scrubAudioEnabled"), on);
        });
        menu.exec(mw.mapToGlobal(QPoint(0, 0)));
    };
    canvas_menu->addAction(MainWindow::tr("&Preferences..."), QKeySequence::Preferences,
                           &mw, show_preferences);
    canvas_menu->addSeparator();
    canvas_menu->addAction(MainWindow::tr("&Quit Nova Canvas Studio"), QKeySequence::Quit,
                           qApp, &QApplication::quit);

    auto* file = mw.ui->menubar->addMenu(MainWindow::tr("&File"));
    file->addAction(MainWindow::tr("&New Project"), QKeySequence::New, &mw, &MainWindow::on_new_project);
    file->addAction(MainWindow::tr("&Open Project..."), QKeySequence::Open, &mw, &MainWindow::on_open_project);
    mw.open_recent_menu_ = file->addMenu(MainWindow::tr("Open &Recent"));
    mw.open_recent_menu_->setEnabled(false);
    QObject::connect(mw.open_recent_menu_, &QMenu::triggered, &mw, &MainWindow::on_open_recent_file);
    file->addAction(MainWindow::tr("&Save Project"), QKeySequence::Save, &mw, &MainWindow::on_save_project);
    file->addAction(MainWindow::tr("Save Project &As..."), QKeySequence::SaveAs, &mw, &MainWindow::on_save_project_as);
    file->addSeparator();
    file->addAction(MainWindow::tr("&Import Media..."), QKeySequence(Qt::CTRL | Qt::Key_I), &mw,
                    &MainWindow::on_import_media);

    auto* edit = mw.ui->menubar->addMenu(MainWindow::tr("&Edit"));
    edit->addAction(MainWindow::tr("&Undo"), QKeySequence::Undo, &mw, &MainWindow::on_undo);
    edit->addAction(MainWindow::tr("&Redo"), QKeySequence::Redo, &mw, &MainWindow::on_redo);
<<<<<<< Updated upstream
    edit->addSeparator();
    edit->addAction(MainWindow::tr("&Preferences..."), QKeySequence::Preferences, &mw, [&mw] {
                    // Audible-scrubbing preference. A small popup menu keeps the
                    // option discoverable without a dedicated settings dialog.
                    QMenu menu;
            apply_rounded_menu(&menu);
                    const bool saved = QSettings().value(QStringLiteral("scrubAudioEnabled"), true).toBool();
                    mw.controller_.set_scrub_audio_enabled(saved);
                    auto* scrub_audio = menu.addAction(MainWindow::tr("Audible Scrubbing"));
                    scrub_audio->setCheckable(true);
                    scrub_audio->setChecked(saved);
                    QObject::connect(scrub_audio, &QAction::toggled, &mw, [&mw](bool on) {
                        mw.controller_.set_scrub_audio_enabled(on);
                        QSettings().setValue(QStringLiteral("scrubAudioEnabled"), on);
                    });
                    menu.exec(mw.mapToGlobal(QPoint(0, 0)));
                });

=======
>>>>>>> Stashed changes
    auto* trim = mw.ui->menubar->addMenu(MainWindow::tr("&Trim"));
    trim->addAction(MainWindow::tr("Ripple Delete"), QKeySequence(Qt::Key_Delete), &mw,
                    [&mw] { mw.delete_selected_clip(/*ripple=*/true); });
    trim->addAction(MainWindow::tr("Lift"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), &mw,
                    [&mw] { mw.delete_selected_clip(/*ripple=*/false); });
    trim->addAction(MainWindow::tr("Cycle Edit Point Side"), QKeySequence(Qt::Key_U));
    trim->addAction(MainWindow::tr("Remove All Transitions"), &mw, [&mw] {
        mw.remove_all_transitions();
    });

    auto* timeline_menu = mw.ui->menubar->addMenu(MainWindow::tr("&Timeline"));
    timeline_menu->addAction(MainWindow::tr("Add Edit"), QKeySequence(Qt::CTRL | Qt::Key_Backslash));
    timeline_menu->addAction(MainWindow::tr("Add Marker"), QKeySequence(Qt::Key_M));
    timeline_menu->addAction(MainWindow::tr("Zoom to Fit"), QKeySequence(Qt::SHIFT | Qt::Key_Z), &mw,
                             [&mw] { mw.timeline_->zoom_fit(); });

    auto* clip_menu = mw.ui->menubar->addMenu(MainWindow::tr("&Clip"));
    clip_menu->addAction(MainWindow::tr("Add Transition"), QKeySequence(Qt::CTRL | Qt::Key_T));
    clip_menu->addAction(MainWindow::tr("Link/Unlink"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L));

    // "Clip Colour >" submenu: the full Resolve-style pinch wheel applied to the
    // currently selected clip. Each entry carries its 1-12 swatch index (0 = no
    // colour) and funnels into the SAME edit op the Inspector + context menu use.
    auto* clip_color_menu = clip_menu->addMenu(MainWindow::tr("Clip Colour") + QStringLiteral(" >"));
    apply_rounded_menu(clip_color_menu);
    const auto swatch_action = [&mw](uint8_t color) {
        QAction* act = new QAction(&mw);
        act->setData(color);
        QObject::connect(act, &QAction::triggered, &mw,
                         [&mw, color]() { mw.apply_clip_color(color); });
        return act;
    };
    const QColor* swatches = clip_color_swatches();
    for (int i = 0; i < 12; ++i) {
        QAction* act = swatch_action(static_cast<uint8_t>(i + 1));
        act->setText(QStringLiteral("#%1").arg(swatches[i].name()));
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QBrush(swatches[i]));
        p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
        p.drawRoundedRect(QRectF(0.5, 0.5, 15, 15), 3, 3);
        act->setIcon(QIcon(pm));
        clip_color_menu->addAction(act);
    }
    clip_color_menu->addSeparator();
    QAction* no_color = swatch_action(0);
    no_color->setText(MainWindow::tr("&No Colour"));
    clip_color_menu->addAction(no_color);
    // Only meaningful when a clip is selected; the whole submenu enables with it.
    QObject::connect(clip_menu, &QMenu::aboutToShow, &mw, [clip_color_menu, &mw]() {
        clip_color_menu->setEnabled(mw.selected_clip_ != 0);
    });

    auto* mark_menu = mw.ui->menubar->addMenu(MainWindow::tr("&Mark"));
    mark_menu->addAction(MainWindow::tr("Mark In"), QKeySequence(Qt::Key_I));
    mark_menu->addAction(MainWindow::tr("Mark Out"), QKeySequence(Qt::Key_O));
    mark_menu->addAction(MainWindow::tr("Clear In/Out"), QKeySequence(Qt::ALT | Qt::Key_X));

    auto* view = mw.ui->menubar->addMenu(MainWindow::tr("&View"));
    auto* inspector_toggle_action = view->addAction(MainWindow::tr("&Inspector"), QKeySequence(Qt::Key_I), &mw, [&mw] {
        if (mw.inspector_dock_) mw.inspector_dock_->setVisible(!mw.inspector_dock_->isVisible());
    });
    inspector_toggle_action->setCheckable(true);
    mw.inspector_toggle_action_ = inspector_toggle_action;
    view->addAction(MainWindow::tr("Toggle &Full Screen"), QKeySequence(Qt::Key_F11), &mw,
                    [&mw] { mw.isFullScreen() ? mw.showNormal() : mw.showFullScreen(); });

    auto* playback = mw.ui->menubar->addMenu(MainWindow::tr("Play&back"));
    playback->addAction(MainWindow::tr("&Play/Pause"), QKeySequence(Qt::Key_Space),
                        [&mw] { mw.controller_.toggle_play_pause(); });
    playback->addAction(MainWindow::tr("Previous &Frame"), QKeySequence(Qt::Key_Left),
                        [&mw] { mw.controller_.pause(); mw.controller_.step(-1); });
    playback->addAction(MainWindow::tr("&Next Frame"), QKeySequence(Qt::Key_Right),
                        [&mw] { mw.controller_.pause(); mw.controller_.step(1); });
    playback->addAction(MainWindow::tr("Go &to Start"), QKeySequence(Qt::Key_Home),
                        [&mw] { mw.controller_.seek(0); });
    playback->addAction(MainWindow::tr("Go &to End"), QKeySequence(Qt::Key_End),
                        [&mw] { mw.controller_.seek(mw.total_frames_ - 1); });

    auto* appearance = mw.ui->menubar->addMenu(MainWindow::tr("A&ppearance"));
    auto* appearance_group = new QActionGroup(appearance);
    appearance_group->setExclusive(true);
    const bool light_mode = is_light();
    auto* dark_action = appearance->addAction(MainWindow::tr("&Dark"));
    dark_action->setCheckable(true);
    dark_action->setChecked(!light_mode);
    auto* light_action = appearance->addAction(MainWindow::tr("&Light"));
    light_action->setCheckable(true);
    light_action->setChecked(light_mode);
    appearance_group->addAction(dark_action);
    appearance_group->addAction(light_action);
    const auto persist_mode = [](bool light) {
        set_light(light);
        QSettings().setValue(QStringLiteral("appearance/theme"),
                             light ? QStringLiteral("light") : QStringLiteral("dark"));
    };
    QObject::connect(dark_action, &QAction::triggered, &mw,
                     [persist_mode] { persist_mode(false); });
    QObject::connect(light_action, &QAction::triggered, &mw,
                     [persist_mode] { persist_mode(true); });

    for (const char* name : {"Fusion", "Color", "Fairlight", "Workspace", "Help"}) {
        auto* m = mw.ui->menubar->addMenu(MainWindow::tr(name));
        if (qstrcmp(name, "Help") == 0) {
            QAction* help_item = m->addAction(MainWindow::tr("Nova Canvas Studio Help"));
            help_item->setEnabled(false);  // stub until real docs exist
        } else if (qstrcmp(name, "Workspace") == 0) {
            m->addAction(MainWindow::tr("Reset UI Layout"));
        } else {
            m->setEnabled(false);
        }
    }

    // Round every menubar dropdown (and any submenu, e.g. Open Recent). Must
    // run after the menus are populated and before any is shown.
    std::function<void(QMenu*)> round_menu_tree = [&](QMenu* menu) {
        if (!menu) return;
        apply_rounded_menu(menu);
        const auto actions = menu->actions();
        for (QAction* act : actions)
            if (QMenu* sub = act->menu()) round_menu_tree(sub);
    };
    const auto bar_actions = mw.ui->menubar->actions();
    for (QAction* act : bar_actions)
        round_menu_tree(act->menu());
}

}  // namespace canvas::gui