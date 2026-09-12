#include "UX/theme_menu.hpp"

#include "UX/theme_state.hpp"

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QObject>
#include <QString>
#include <QWidget>

namespace canvas::gui {

namespace {

void apply_rounded_menu_impl(QMenu* menu) {
    if (!menu) return;
    // Translucent + frameless make the popup's QSS border-radius really clip;
    // without them the native popup window keeps square corners.
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
    menu->setWindowFlag(Qt::FramelessWindowHint, true);
    apply_panel_shadow(menu);
}

// Safety net for popups not created through make_rounded_menu: menubar-owned
// submenus and QComboBox dropdown containers. QEvent::Polish fires on creation
// (before the native window exists), Show is a last-chance retry.
class PopupRounder : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() != QEvent::Show && e->type() != QEvent::Polish)
            return false;
        if (auto* menu = qobject_cast<QMenu*>(obj)) {
            apply_rounded_menu_impl(menu);
            return false;
        }
        if (QWidget* w = qobject_cast<QWidget*>(obj)) {
            if (!w->isWindow()) return false;
            if (QString::fromLatin1(w->metaObject()->className()) !=
                QLatin1String("QComboBoxPrivateContainer"))
                return false;
            w->setAttribute(Qt::WA_TranslucentBackground, true);
            w->setWindowFlag(Qt::FramelessWindowHint, true);
        }
        return false;
    }
};

}  // namespace

QMenu* make_rounded_menu(QWidget* parent) {
    auto* menu = new QMenu(parent);
    apply_rounded_menu_impl(menu);
    return menu;
}

void apply_rounded_menu(QMenu* menu) {
    apply_rounded_menu_impl(menu);
}

void install_popup_rounding(QApplication& app) {
    app.installEventFilter(new PopupRounder(&app));
}

}  // namespace canvas::gui