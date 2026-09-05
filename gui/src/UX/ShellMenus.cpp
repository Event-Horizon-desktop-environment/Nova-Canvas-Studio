#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QSettings>

namespace canvas::gui {

void build_app_menus(MainWindow& mw) {
    // 1. MENU BAR — full Resolve-style row (File/Edit/Trim/Timeline/Clip/
    //    Mark/View/Playback/Fusion/Color/Fairlight/Workspace/Help). Only
    //    File/Edit/Playback/View/Timeline/Mark are wired for v1; the rest
    //    are present with stub items so the chrome matches the reference.
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
    file->addSeparator();
    file->addAction(MainWindow::tr("E&xit"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto* edit = mw.ui->menubar->addMenu(MainWindow::tr("&Edit"));
    edit->addAction(MainWindow::tr("&Undo"), QKeySequence::Undo, &mw, &MainWindow::on_undo);
    edit->addAction(MainWindow::tr("&Redo"), QKeySequence::Redo, &mw, &MainWindow::on_redo);
    edit->addSeparator();
    edit->addAction(MainWindow::tr("&Preferences..."), QKeySequence::Preferences, &mw, [&mw] {
                    // Audible-scrubbing preference. A small popup menu keeps the
                    // option discoverable without a dedicated settings dialog.
                    QMenu menu;
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

    auto* trim = mw.ui->menubar->addMenu(MainWindow::tr("&Trim"));
    trim->addAction(MainWindow::tr("Ripple Delete"), QKeySequence(Qt::Key_Delete), &mw,
                    [&mw] { mw.delete_selected_clip(/*ripple=*/true); });
    trim->addAction(MainWindow::tr("Lift"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), &mw,
                    [&mw] { mw.delete_selected_clip(/*ripple=*/false); });
    trim->addAction(MainWindow::tr("Cycle Edit Point Side"), QKeySequence(Qt::Key_U));

    auto* timeline_menu = mw.ui->menubar->addMenu(MainWindow::tr("&Timeline"));
    timeline_menu->addAction(MainWindow::tr("Add Edit"), QKeySequence(Qt::CTRL | Qt::Key_Backslash));
    timeline_menu->addAction(MainWindow::tr("Add Marker"), QKeySequence(Qt::Key_M));
    timeline_menu->addAction(MainWindow::tr("Zoom to Fit"), QKeySequence(Qt::SHIFT | Qt::Key_Z), &mw,
                             [&mw] { mw.timeline_->zoom_fit(); });

    auto* clip_menu = mw.ui->menubar->addMenu(MainWindow::tr("&Clip"));
    clip_menu->addAction(MainWindow::tr("Add Transition"), QKeySequence(Qt::CTRL | Qt::Key_T));
    clip_menu->addAction(MainWindow::tr("Link/Unlink"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L));

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

    for (const char* name : {"Fusion", "Color", "Fairlight", "Workspace", "Help"}) {
        auto* m = mw.ui->menubar->addMenu(MainWindow::tr(name));
        if (qstrcmp(name, "Help") == 0) {
            m->addAction(MainWindow::tr("About Nova Canvas Studio"));
        } else if (qstrcmp(name, "Workspace") == 0) {
            m->addAction(MainWindow::tr("Reset UI Layout"));
        } else {
            m->setEnabled(false);
        }
    }
}

}  // namespace canvas::gui