// Page-switcher toolbar (Media/Cut/Edit/Fusion/Color/Fairlight/Deliver + the
// Home/Settings cluster), split out of ShellTopBar.cpp (splitplan refactor).
// Each page button routes to enter_deliver_page / enter_edit_page plus the
// Color page entry-exit seam; everything else falls back to the Edit layout.

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
            // Edit, Deliver, and Color have their own layouts; the rest fall
            // back to the Edit workspace. Leaving the Color page is handled
            // here for whatever target page the bar lands on.
            const bool deliver = qstrcmp(name, "Deliver") == 0;
            const bool color_page = qstrcmp(name, "Color") == 0;
            for (QToolButton* other : page_bar->findChildren<QToolButton*>()) {
                if (other != b) other->setChecked(false);
            }
            b->setChecked(true);
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

}  // namespace canvas::gui