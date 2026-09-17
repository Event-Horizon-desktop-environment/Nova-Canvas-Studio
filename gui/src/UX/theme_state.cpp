#include "UX/theme_state.hpp"

#include "UX/horizon_style.hpp"
#include "UX/theme_menu.hpp"
#include "UX/theme_tokens.hpp"

#include <QApplication>
#include <QColor>
#include <QGraphicsDropShadowEffect>
#include <QPalette>
#include <QPixmapCache>
#include <QPointer>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

#include <utility>

namespace canvas::gui {

namespace {

bool g_light = false;
bool g_hypr_dark = false;
QApplication* g_app = nullptr;
std::vector<std::function<void()>> g_reapply;

QString make_flat_controls_qss() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
      "QWidget { color: %1; }"
      "QToolTip { background-color: %2; color: %1; border: 1px solid %3;"
      " padding: 4px 8px; border-radius: 8px; }"
      "QMenuBar { background-color: %15; color: %1;"
      "  padding: 0 4px; }"
      "QMenuBar::item { background: transparent; border-radius: 6px;"
      "  padding: 6px 12px; margin: 4px 1px; font-size: 14px; }"
      "QMenuBar::item:selected { background: %4; }"
      "QMenuBar::item:pressed { background: %5; }"
      "QDockWidget { background: transparent; color: %1; }"
      "QMenu { background-color: %16; color: %1;"
      "  border: 1px solid %3;"
      "  border-radius: 8px; padding: 6px; "
      "  font-size: 14px; }"
      "QMenu::item { padding: 7px 28px 7px 12px; border-radius: 7px;"
      "  margin: 1px 3px 1px 4px; }"
      "QMenu::item:selected { background: %4; color: %1; }"
      "QMenu::item:selected:disabled { background: transparent; color: %6; }"
      "QMenu::item:checked { color: %8; font-weight: 600; }"
      "QMenu::item:disabled { color: %6; }"
      "QMenu::separator { height: 1px; background: %3; margin: 6px 12px; }"
      "QMenu::separator:horizontal { height: 1px; }"
      "QMenu::indicator { width: 16px; height: 16px; margin: 0 2px; }"
      "QMenu::indicator:checked { background: %13; border-radius: 4px;"
      "  image: url(:/icons/check.svg); }"
      "QMenu::right-arrow { image: url(:/icons/chevron_right.svg);"
      "  width: 12px; height: 12px; margin-right: 5px; }"
      "QMenu::icon { margin-left: 2px; margin-right: 8px; }"
      "QStatusBar { color: %7; }"
      "QTabBar::tab { background: transparent; color: %7; padding: 6px 14px;"
      "  border-bottom: 2px solid transparent; }"
      "QTabBar::tab:selected { color: %8; border-bottom: 2px solid %9; }"
      "QTabBar::tab:hover { color: %1; }"
      "QTabBar::scroller { width: 32px; }"
      "QTabBar QToolButton { min-width: 18px; min-height: 18px; max-width: 22px;"
      "  max-height: 22px; border-radius: 9px; background: transparent; }"
      "QTabBar QToolButton:hover { background: %4; }"
      "QTabBar QToolButton:pressed { background: %5; }"
      "QScrollArea { background: transparent; border: none; }"
      "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
      "QScrollBar::handle:vertical { background: %10; border-radius: 5px; min-height: 24px; }"
      "QScrollBar::handle:vertical:hover { background: %11; }"
      "QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }"
      "QScrollBar::handle:horizontal { background: %10; border-radius: 5px; min-width: 24px; }"
      "QScrollBar::handle:horizontal:hover { background: %11; }"
      "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
      "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
      "QSlider::groove:horizontal { height: 3px; background: %10; border-radius: 1.5px; }"
      "QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: %9;"
      "  border: none; border-radius: 7px; }"
      "QSlider::handle:horizontal:hover { background: %12; }"
      "QSlider::handle:horizontal:pressed { background: %9; }"
      "QSlider::sub-page:horizontal { background: %12; border-radius: 1.5px; }"
      "QCheckBox, QRadioButton { spacing: 8px; outline: none; }"
      "QCheckBox::indicator, QRadioButton::indicator { width: 16px; height: 16px;"
      "  background: %10; border: 1px solid %3; border-radius: 5px; }"
      "QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %12; }"
      "QCheckBox::indicator:checked { background: %13; border-color: %13;"
      "  image: url(:/icons/check.svg); }"
      "QComboBox { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 4px 10px; }"
      "QComboBox:hover { background: %11; }"
      "QComboBox QAbstractItemView { background: %2; border: 1px solid %3;"
      "  selection-background-color: %13; selection-color: %14; outline: none;"
      "  border-radius: 6px; padding: 4px; }"
      "QLineEdit { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 3px 8px; }"
      "QLineEdit:focus { border-color: %12; }"
      "QAbstractScrollArea::corner { background: transparent; border: none; }"
      "QDoubleSpinBox, QSpinBox { background: %10; color: %1; border: 1px solid %3;"
      "  border-radius: 8px; padding: 3px 8px; }"
      "QDoubleSpinBox:focus, QSpinBox:focus, QDoubleSpinBox:hover, QSpinBox:hover"
      "  { border-color: %12; }"
      "QDoubleSpinBox::up-button, QSpinBox::up-button,"
      "QDoubleSpinBox::down-button, QSpinBox::down-button { width: 16px;"
      "  background: transparent; border: none; margin: 1px; }"
    )
      .arg(css(t.ink), css(t.surface_highest), css(t.border),
           css(t.state_hover), css(t.state_selected), css(t.ink_faint),
           css(t.ink_muted), css(t.accent_text),
css(t.accent), css(t.surface_higher), css(t.surface_highest),
        css(t.accent_hover), css(t.accent_press), css(t.on_accent),
        css(t.surface), css(t.surface_raised));
}

QPalette makeHorizonPalette() {
    const ThemeTokens& t = tokens();
    QPalette p;
    p.setColor(QPalette::Window,        t.surface);
    p.setColor(QPalette::WindowText,    t.ink);
    p.setColor(QPalette::Base,          t.surface_low);
    p.setColor(QPalette::AlternateBase, t.surface_raised);
    p.setColor(QPalette::Text,          t.ink);
    p.setColor(QPalette::Button,        t.surface_raised);
    p.setColor(QPalette::ButtonText,    t.ink);
    p.setColor(QPalette::BrightText,    t.danger);
    p.setColor(QPalette::Highlight,     t.accent);
    p.setColor(QPalette::HighlightedText, t.on_accent);
    p.setColor(QPalette::PlaceholderText, t.ink_faint);
    p.setColor(QPalette::ToolTipBase,   t.surface_highest);
    p.setColor(QPalette::ToolTipText,   t.ink);
    p.setColor(QPalette::Link,          t.accent_text);
    p.setColor(QPalette::Disabled, QPalette::Text,       t.ink_faint);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.ink_faint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.ink_faint);
    p.setColor(QPalette::Disabled, QPalette::Highlight,  t.surface_higher);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.ink_faint);
    return p;
}

void reapply_theme() {
    g_app->setPalette(makeHorizonPalette());
    g_app->setStyleSheet(make_flat_controls_qss());
    QPixmapCache::clear();
    for (QWidget* w : g_app->allWidgets()) {
        w->update();
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
    for (const auto& fn : g_reapply)
        fn();
}

}

bool is_light() { return g_light; }

void set_light(bool light) {
    if (g_light == light)
        return;
    g_light = light;
    if (g_app) {
        reapply_theme();
    } else {
        for (const auto& fn : g_reapply)
            fn();
    }
}

bool is_hypr_dark() { return g_hypr_dark; }

void set_hypr_dark(bool enabled) {
    if (g_hypr_dark == enabled)
        return;
    g_hypr_dark = enabled;
    if (g_app) {
        reapply_theme();
    } else {
        for (const auto& fn : g_reapply)
            fn();
    }
}

void refresh_theme() {
    if (g_app) {
        reapply_theme();
    } else {
        for (const auto& fn : g_reapply)
            fn();
    }
}

void register_theme_reapply(std::function<void()> fn) {
    g_reapply.push_back(std::move(fn));
}

void apply_theme_style(QWidget* w, const std::function<QString()>& style) {
    w->setStyleSheet(style());
    register_theme_reapply([wp = QPointer<QWidget>(w), style] {
        if (wp)
            wp->setStyleSheet(style());
    });
}

void apply_panel_shadow(QWidget* w) {
    auto* effect = new QGraphicsDropShadowEffect(w);
    effect->setBlurRadius(44);
    effect->setOffset(0, 8);
    effect->setColor(QColor(0, 0, 0, is_light() ? 70 : 130));
    w->setGraphicsEffect(effect);
    register_theme_reapply([wp = QPointer<QWidget>(w)] {
        if (!wp)
            return;
        if (auto* e = qobject_cast<QGraphicsDropShadowEffect*>(wp->graphicsEffect()))
            e->setColor(QColor(0, 0, 0, is_light() ? 70 : 130));
    });
}

void apply_theme(QApplication& app, bool light, bool hypr_dark) {
    g_app = &app;
    g_light = light;
    g_hypr_dark = hypr_dark;
    QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    app.setStyle(new HorizonStyle(fusion));
    app.setPalette(makeHorizonPalette());
    app.setStyleSheet(make_flat_controls_qss());
    install_popup_rounding(app);
}

}
