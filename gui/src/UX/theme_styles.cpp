#include "UX/theme_styles.hpp"

#include "UX/theme_tokens.hpp"

namespace canvas::gui {

QString transport_bar_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "background-color: %1; border-top: 1px solid %2;")
        .arg(css(t.surface), css(t.border_soft));
}

QString timeline_tools_style() {
    return QStringLiteral("background-color: transparent;");
}

QString page_switcher_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "background-color: %1; border-top: 1px solid %2;")
        .arg(css(t.surface), css(t.border_soft));
}

QString time_label_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("font-family: %1; color: %2;")
        .arg(t.font_mono, css(t.ink));
}

QString media_pool_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QListWidget { background-color: %1; border: 1px solid %2;"
        " border-radius: 0px; }"
        "QListWidget::item { background: transparent; color: %3; padding: 0px;"
        "  border: none; margin: 0px; }"
        "QListWidget::item:hover { background: transparent; }"
        "QListWidget::item:selected { background: transparent; color: %3; }")
        .arg(css(t.surface_low), css(t.border), css(t.ink));
}

QString viewer_frame_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QFrame#viewerFrame { background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 0px; }")
        .arg(css(t.surface_low), css(t.border));
}

QString timeline_frame_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QFrame#timelineFrame { background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 0px; }")
        .arg(css(t.surface), css(t.border));
}

QString dock_glow_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("QDockWidget { background-color: %1; }")
        .arg(css(t.surface));
}

QString dock_panel_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#dockGlassCard { background-color: %1;"
        " border: none;"
        " border-left: 1px solid %2;"
        " border-right: 1px solid %2;"
        " border-bottom: 1px solid %2;"
        " border-radius: 0px; }")
        .arg(css(t.surface_raised), css(t.border));
}

QString global_toolbar_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("background-color: %1;").arg(css(t.surface));
}

QString top_status_bar_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "background-color: %1; border-bottom: 1px solid %2;")
        .arg(css(t.surface), css(t.border));
}

QString big_timecode_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("font-family: %1; font-size: 20px; font-weight: 500; color: %2; background: transparent;")
        .arg(t.font_mono, css(t.ink));
}

QString bin_tree_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QTreeWidget { background-color: %1; border: none; color: %2; }"
        "QTreeWidget::branch { background: transparent; }"
        "QTreeWidget::item { padding: 3px 2px; border-radius: 8px; }"
        "QTreeWidget::item:hover { background-color: %3; }"
        "QTreeWidget::item:selected { background-color: %4; color: %5; }"
        "QTreeWidget::item:selected:hover { background-color: %6; }")
        .arg(css(t.surface), css(t.ink), css(t.state_hover),
             css(t.accent_soft), css(t.accent_text), css(t.accent_soft));
}

QString inspector_category_header_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: transparent; border: none;"
        " padding: 8px 10px; text-align: left; color: %1; font-weight: 600;"
        " border-radius: 6px; }"
        "QToolButton:hover { background-color: %2; }")
        .arg(css(t.ink), css(t.state_hover));
}

QString inspector_card_header_open_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardHeader {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-bottom: none; }")
        .arg(css(t.surface_highest), css(t.border_soft));
}

QString inspector_card_header_closed_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardHeader {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 0px; }")
        .arg(css(t.surface_highest), css(t.border_soft));
}

QString inspector_card_body_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardBody { background-color: %1;"
        " border-left: 1px solid %2; border-right: 1px solid %2;"
        " border-bottom: 1px solid %2; }")
        .arg(css(t.surface_low), css(t.border_soft));
}

QString page_pill_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; padding: 6px 12px; background: transparent;"
        " border: none; border-bottom: 2px solid transparent; }"
        "QToolButton:hover:!checked { color: %2; }"
        "QToolButton:pressed { background-color: %5; }"
        "QToolButton:checked { color: %3; border-bottom-color: %4;"
        " font-weight: 600; }")
        .arg(css(t.ink_muted), css(t.ink), css(t.accent_text), css(t.accent),
             css(t.state_press));
}

QString inspector_tab_track_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget { background-color: transparent;"
        " border-bottom: 1px solid %1; }")
        .arg(css(t.border_soft));
}

QString inspector_tab_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; background: transparent; border: none;"
        " border-bottom: 2px solid transparent; padding: 7px 4px; font-weight: 500; }"
        "QToolButton:hover:!checked { color: %2; }"
        "QToolButton:pressed { background-color: %5; }"
        "QToolButton:checked { color: %3; border-bottom-color: %4;"
        " font-weight: 600; }")
        .arg(css(t.ink_muted), css(t.ink), css(t.accent_text), css(t.accent),
             css(t.state_press));
}

QString left_tab_strip_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QTabWidget#leftTabStrip::pane{background:transparent;border:none;}"
        "QTabBar::tab{background:transparent;color:%1;padding:7px 12px;"
        "  border:none;border-bottom:2px solid transparent;font-weight:500;margin:0 1px;}"
        "QTabBar::tab:hover{color:%2;}"
        "QTabBar::tab:selected{color:%3;border-bottom-color:%4;font-weight:600;}"
        "QTabBar QToolButton{background:transparent;border:none;border-radius:6px;}"
        "QTabBar QToolButton:hover{background:%5;}")
        .arg(css(t.ink_muted), css(t.ink), css(t.accent_text), css(t.accent),
             css(t.state_hover));
}

QString flat_tool_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid transparent;"
        " border-radius: 8px; padding: 5px; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:pressed { background: %2; }"
        "QToolButton:checked { background: %3; border: 1px solid %4; }")
        .arg(css(t.state_hover), css(t.state_press),
             css(t.state_selected), css(t.accent_line));
}

QString transport_tool_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; background-color: %2; border: none;"
        " border-radius: 8px; padding: 5px 7px; }"
        "QToolButton:hover { background-color: %3; }"
        "QToolButton:pressed { background-color: %4; }"
        "QToolButton:checked { background-color: %5; }")
        .arg(css(t.ink), css(t.surface_raised), css(t.surface_higher),
             css(t.state_press), css(t.accent_soft));
}

QString tool_cluster_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid transparent;"
        " border-radius: 8px; padding: 5px; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:checked { background: %2; border: 1px solid %3; }")
        .arg(css(t.state_hover), css(t.state_selected), css(t.accent_line));
}

QString outline_pill_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; background: transparent; border: 1px solid %2;"
        " border-radius: 8px; padding: 3px 14px; font-size: 12px; }"
        "QToolButton:hover { border-color: %3; color: %4; background: %5; }"
        "QToolButton:checked { border-color: %6; color: %6; background: %7; }")
        .arg(css(t.ink_muted), css(t.border),
             css(t.ink_muted), css(t.ink), css(t.state_hover),
             css(t.danger), css(t.danger_soft));
}

QString slider_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; background: %3;"
        " border: none; border-radius: 4px; }"
        "QSlider::handle:horizontal:hover { background: %4; }")
        .arg(css(t.border), css(t.accent), css(t.ink), css(t.surface_highest));
}

QString transport_play_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background-color: %1; border: 1px solid %2;"
        " border-radius: 50%; min-width: 30px;"
        " max-width: 30px; min-height: 30px; max-height: 30px; }"
        "QToolButton:hover { background-color: %3; }"
        "QToolButton:pressed { background-color: %4; }")
        .arg(css(t.accent), css(t.accent_line), css(t.accent_hover),
             css(t.accent_press));
}

}
