#include "UX/theme_styles.hpp"

#include "UX/theme_tokens.hpp"

namespace canvas::gui {

// ---------------------------------------------------------------------------
// App-specific chrome. These per-widget stylesheets layer token variants on
// top of the global sheet and HorizonStyle, applied to individual widgets.
// ---------------------------------------------------------------------------
QString transport_bar_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "background-color: %1; border-top: 1px solid %2;")
        .arg(css(t.surface), css(t.border_soft));
}

QString timeline_tools_style() {
    return QStringLiteral("background-color: transparent;");
}

// The page-switcher strip: flat workspace surface — the page buttons above
    // carry their own underline states, so the bar itself stays invisible.
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

// Flat monitor bezel: the viewer reads as an instrument — a flush, square dark
    // well with a single hairline rim. No float, no shadow, no catch-light.
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

// Flat dock backdrop: no gradient glow, just the workspace surface.
QString dock_glow_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("QDockWidget { background-color: %1; }")
        .arg(css(t.surface));
}

// Raised edge-to-edge panel that carries a dock's content: a solid raised
// surface distinct from the flat workspace (grease-screen hierarchy — the
// dock read as chrome, the centre stays deep) inside a strong 1px rim on the
// sides and bottom. The TOP edge deliberately has no border — directly under
// the (transparent) dock title bar it would sit as an orphan line above the
// tab strip, and on Hyprland's colour pass a bare hairline picks up a stray
// green cast.
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

// Semi-rounded inspector category card: the header is the raised top band (or
// the whole card when collapsed), the body the inset content well. Radii match
// the card shape; the seam between the two is the header's bottom hairline.
QString inspector_category_header_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: transparent; border: none;"
        " padding: 8px 10px; text-align: left; color: %1; font-weight: 600;"
        " border-radius: 6px; }"
        "QToolButton:hover { background-color: %2; }")
        .arg(css(t.ink), css(t.state_hover));
}

// Category section: a flat row, not a card. OPEN shows the raised top band
// with a hairline all round except the seam; CLOSED is a plain outlined
// rectangle. Squared — categories read as stacked bands separated by
// hairlines, the way a property editor stacks its sections.
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

// Page switcher + top-bar action buttons share the flat tab idiom: resting
// state is bare text, the active/checked member carries a 2px underline in the
// Nova-gold accent. Chrome reads as a continuous header strip, not pills.
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

// Inspector mode row: a flat header strip — the track is invisible and the
// active mode is a text button with the amber underline, like the page bar.
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

// Left "Media Pool / Sync Bin / ..." tab strip: the same flat underline idiom
// as the page bar — no pills, no filled segments.
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

// Flat icon toolbar buttons — idle state is invisible (no fill, no border);
// hover/press/checked reveal a plain flat surface. This is the macOS toolbar
// idiom: chrome disappears until you touch it, instead of every icon sitting
// in its own bordered box.
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

// "Bracket-pill" edit-tool cluster (select/trim/blade/mode): flat members,
// active member shows a plain accent fill — no idle border.
QString tool_cluster_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid transparent;"
        " border-radius: 8px; padding: 5px; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:checked { background: %2; border: 1px solid %3; }")
        .arg(css(t.state_hover), css(t.state_selected), css(t.accent_line));
}

// Outlined button (DIM) — flat surface, single hairline border, danger tint
// only once actually engaged. Caption-scale label so it stays compact in the
// toolbar.
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

// Flat sliders: pill groove, weatherproof squircle knob — a fader, not a dot.
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

// Circumferential Nova-gold play button: a round amber disc — the one
// prominent action in the transport, flat, no bevel.
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

}  // namespace canvas::gui