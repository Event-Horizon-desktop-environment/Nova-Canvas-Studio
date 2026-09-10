#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include <QDockWidget>

namespace canvas::gui {

// build_ui() is the ordered coordinator for the programmatic UI shell. Each
// builder lives in its own .cpp and populates the .ui-supplied docks/chrome:
//   ShellMenus.cpp    build_app_menus      — File/Edit/Trim/.../Help menu bar
//   ShellTopBar.cpp   build_top_bar, build_page_bar, build_transport_bar
//   ShellDocks.cpp    build_left_dock, build_inspector_dock
//   ShellCenter.cpp   build_center_workspace — viewer column, timeline, deliver
void MainWindow::build_ui() {
    // The .ui file owns the window shell: central widget, menu bar, status bar
    // and the three dock widgets (media pool left, inspector right, timeline
    // bottom). Everything below populates those widgets with app chrome.
    ui = new Ui::MainWindow;
    // Claim the four corners BEFORE setupUi so the .ui's addDockWidget calls
    // lay the docks out the proven way: the media pool owns the bottom-left
    // corner (a full-height left column) and the timeline docks into the
    // bottom-center — starting at the media pool's right edge, NOT spanning
    // underneath it. Resolve-style collapse depends on this: hiding the media
    // pool panel shrinks the left dock to a thin strip, which frees its
    // horizontal span and slides the timeline + viewer LEFT — the pool's
    // collapse hands the room it occupied to the timeline.
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    ui->setupUi(this);

    // 1. MENU BAR — extracted to ShellMenus.cpp.
    build_app_menus(*this);

    // 2. PAGE-MODE FOUNDATION BAR — extracted to ShellTopBar.cpp.
    build_page_bar(*this);

    // 4. LEFT (bin tree + Media Pool) + 5. RIGHT (INSPECTOR) — extracted to ShellDocks.cpp.
    build_left_dock(*this);
    build_inspector_dock(*this);

    // 6. CENTER (viewer column) + 7. TIMELINE + 7b. DELIVER docks — ShellCenter.cpp.
    build_center_workspace(*this);
}

}  // namespace canvas::gui