#include "UX/MainWindow.hpp"
#include "ui_MainWindow.h"

#include <QDockWidget>

namespace canvas::gui {

void MainWindow::build_ui() {
    ui = new Ui::MainWindow;
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    ui->setupUi(this);

    build_app_menus(*this);

    build_page_bar(*this);

    build_left_dock(*this);
    build_inspector_dock(*this);

    build_center_workspace(*this);

    if (media_dock_)
        resizeDocks({media_dock_}, {620}, Qt::Horizontal);
}

}
