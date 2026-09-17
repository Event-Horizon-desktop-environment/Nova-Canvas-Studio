#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

#include "features/color/color_page.hpp"

#include <QSize>
#include <QSizePolicy>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

namespace canvas::gui {

void build_page_bar(MainWindow& mw) {
    auto* page_bar = new QToolBar(MainWindow::tr("Pages"), &mw);
    page_bar->setMovable(false);
    page_bar->setObjectName(QStringLiteral("pageSwitcher"));
    apply_theme_style(page_bar, &page_switcher_style);
    page_bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    page_bar->setIconSize(QSize(16, 16));

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
            const bool deliver = qstrcmp(name, "Deliver") == 0;
            const bool color_page = qstrcmp(name, "Color") == 0;
            for (QToolButton* other : page_bar->findChildren<QToolButton*>()) {
                if (other != b) other->setChecked(false);
            }
            b->setChecked(true);
            mw.leave_project_manager();
            leave_color_page(mw);
            if (deliver) mw.enter_deliver_page();
            else if (color_page) {
                mw.enter_edit_page();
                enter_color_page(mw);
            } else {
                mw.enter_edit_page();
            }
        });
    }

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

    QObject::connect(home_btn, &QToolButton::clicked, &mw, [&mw, page_bar] {
        for (QToolButton* other : page_bar->findChildren<QToolButton*>())
            other->setChecked(false);
        mw.enter_project_manager();
    });
}

}
