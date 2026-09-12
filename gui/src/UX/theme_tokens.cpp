#include "UX/theme_tokens.hpp"

#include "UX/theme_state.hpp"

#include <QDebug>

#include <utility>

namespace canvas::gui {

namespace {

// "Flat studio console" token sets. The interface is drawn to recede so the
// footage is the hero: fully-opaque, warmth-neutral charcoal surfaces stepped
// only by value (no translucency, no gradient, no glass); separation by 1px
// hairlines rather than floating cards; and two chromatic signals that never
// borrow each other's job — amber ("Nova gold", tungsten/film-telecine) for
// interaction/selection, and ice-cyan for the playback readhead. Danger red
// and warn amber stay functional reserve colors. Each mode is designed
// independently per Apple's own guidance — light stays close-toned, dark
// spreads its levels apart — rather than inverted from the other.
ThemeTokens makeTokens(bool light) {
    ThemeTokens t;
    if (light) {
        t.surface        = QColor(0xF2, 0xF2, 0xF4);
        t.surface_low    = QColor(0xFF, 0xFF, 0xFF);
        t.surface_raised = QColor(0xEC, 0xEC, 0xEF);
        t.surface_higher = QColor(0xE2, 0xE2, 0xE6);
        t.surface_highest = QColor(0xD8, 0xD8, 0xDC);
        t.border         = QColor(0, 0, 0, 40);    // rgba(0,0,0,0.16)
        t.border_soft    = QColor(0, 0, 0, 22);    // rgba(0,0,0,0.09)
        t.border_hi      = QColor(0, 0, 0, 40);    // == border (flat, no catch-light)
        t.ink            = QColor(0x1C, 0x1C, 0x1E);
        t.ink_muted      = QColor(0x6E, 0x6E, 0x73);
        t.ink_faint      = QColor(0xA8, 0xA8, 0xAC);
        t.accent         = QColor(0xCC, 0x84, 0x18);  // Nova gold (deeper on light)
        t.accent_hover   = QColor(0xB6, 0x74, 0x13);
        t.accent_press   = QColor(0xA5, 0x67, 0x0D);
        t.on_accent      = QColor(0x21, 0x16, 0x05);  // near-black, warm
        t.accent_text    = QColor(0x9A, 0x5B, 0x00);  // burnt amber text
        t.playhead       = QColor(0x2F, 0x8F, 0xB4);  // ice-cyan readhead
        t.clip_video     = QColor(0xCB, 0xD4, 0xDF);
        t.clip_audio     = QColor(0xCF, 0xD6, 0xDA);
        t.clip_label     = QColor(0xA6, 0xB0, 0xBC);
        t.clip_border_video = QColor(0x9C, 0xA8, 0xB4);
        t.clip_border_audio = QColor(0x8E, 0xA3, 0x88);
        t.clip_shadow    = QColor(0, 0, 0, 40);
        t.danger         = QColor(0xE5, 0x48, 0x3F);
        t.warn           = QColor(0xE0, 0x7A, 0x00);
        t.focus_ring     = QColor(0x9A, 0x5B, 0x00);
        t.font_ui        = QStringLiteral("Geist");
        t.font_mono      = QStringLiteral("Geist Mono");
    } else {
        t.surface        = QColor(0x16, 0x16, 0x18);
        t.surface_low    = QColor(0x10, 0x10, 0x12);  // monitor well / input wells
        t.surface_raised = QColor(0x1E, 0x1E, 0x21);
        t.surface_higher = QColor(0x28, 0x28, 0x2C);
        t.surface_highest = QColor(0x31, 0x31, 0x36);
        t.border         = QColor(255, 255, 255, 32);  // rgba(255,255,255,0.13)
        t.border_soft    = QColor(255, 255, 255, 20);  // rgba(255,255,255,0.08)
        t.border_hi      = QColor(255, 255, 255, 32);  // == border (flat, no catch-light)
        t.ink            = QColor(0xED, 0xED, 0xF0);
        t.ink_muted      = QColor(0x9A, 0x9A, 0xA0);
        t.ink_faint      = QColor(0x64, 0x64, 0x6A);
        t.accent         = QColor(0xE8, 0xA1, 0x3C);  // Nova gold
        t.accent_hover   = QColor(0xF0, 0xAC, 0x4C);
        t.accent_press   = QColor(0xC8, 0x88, 0x29);
        t.on_accent      = QColor(0x21, 0x16, 0x05);  // near-black, warm
        t.accent_text    = QColor(0xFF, 0xC1, 0x66);  // bright amber text on dark
        t.playhead       = QColor(0x5C, 0xC8, 0xE4);  // ice-cyan readhead
        t.clip_video     = QColor(0x2B, 0x2F, 0x35);
        t.clip_audio     = QColor(0x4E, 0x56, 0x61);
        t.clip_label     = QColor(0x8C, 0x92, 0x9C);
        t.clip_border_video = QColor(0x52, 0x5A, 0x64);
        t.clip_border_audio = QColor(0x6F, 0x84, 0x6E);
        t.clip_shadow    = QColor(0, 0, 0, 78);
        t.danger         = QColor(0xFF, 0x50, 0x48);
        t.warn           = QColor(0xFF, 0x9F, 0x0A);
        t.focus_ring     = QColor(0xFF, 0xC1, 0x66);
        t.font_ui        = QStringLiteral("Geist");
        t.font_mono      = QStringLiteral("Geist Mono");
    }
    t.accent_soft    = with_alpha(t.accent, 36);
    t.accent_line    = with_alpha(t.accent, 110);
    t.playhead_soft  = with_alpha(t.playhead, 64);
    t.danger_soft    = with_alpha(t.danger, 38);
    t.state_hover    = with_alpha(t.ink, 14);
    t.state_press    = with_alpha(t.ink, 22);
    t.state_selected = with_alpha(t.accent, 46);
    return t;
}

// "HyprDark": the Dark palette pre-lifted to cancel Hyprland's native-Wayland
// FP16 sRGB re-quantization. Hyprland runs every native surface (tagged or not)
// through that pipeline while XWayland is blitted raw, which is what makes the
// running app read ~2 steps darker with flattened blue. Lifting the base set by
// the measured shift has the compositor's pass land exactly where Dark was
// designed. Only ever active when main() detects the Hyprland Wayland backend.
namespace {
constexpr int kHyprLiftR = 2;
constexpr int kHyprLiftG = 2;
constexpr int kHyprLiftB = 4;

QColor hypr_lift(const QColor& c) {
    if (c.alpha() == 0)
        return c;
    return QColor(qMin(255, c.red() + kHyprLiftR),
                  qMin(255, c.green() + kHyprLiftG),
                  qMin(255, c.blue() + kHyprLiftB), c.alpha());
}

ThemeTokens makeHyprDarkTokens(const ThemeTokens& dark) {
    ThemeTokens t = dark;
    t.surface         = hypr_lift(t.surface);
    t.surface_low     = hypr_lift(t.surface_low);
    t.surface_raised  = hypr_lift(t.surface_raised);
    t.surface_higher  = hypr_lift(t.surface_higher);
    t.surface_highest = hypr_lift(t.surface_highest);
    t.border          = hypr_lift(t.border);
    t.border_soft     = hypr_lift(t.border_soft);
    t.border_hi       = hypr_lift(t.border_hi);
    t.ink             = hypr_lift(t.ink);
    t.ink_muted       = hypr_lift(t.ink_muted);
    t.ink_faint       = hypr_lift(t.ink_faint);
    t.accent          = hypr_lift(t.accent);
    t.accent_hover    = hypr_lift(t.accent_hover);
    t.accent_press    = hypr_lift(t.accent_press);
    t.on_accent       = hypr_lift(t.on_accent);
    t.accent_text     = hypr_lift(t.accent_text);
    t.accent_soft     = hypr_lift(t.accent_soft);
    t.accent_line     = hypr_lift(t.accent_line);
    t.playhead        = hypr_lift(t.playhead);
    t.playhead_soft   = hypr_lift(t.playhead_soft);
    t.clip_video      = hypr_lift(t.clip_video);
    t.clip_audio      = hypr_lift(t.clip_audio);
    t.clip_label      = hypr_lift(t.clip_label);
    t.clip_border_video = hypr_lift(t.clip_border_video);
    t.clip_border_audio = hypr_lift(t.clip_border_audio);
    t.danger          = hypr_lift(t.danger);
    t.danger_soft     = hypr_lift(t.danger_soft);
    t.warn            = hypr_lift(t.warn);
    t.state_hover     = hypr_lift(t.state_hover);
    t.state_press     = hypr_lift(t.state_press);
    t.state_selected  = hypr_lift(t.state_selected);
    t.focus_ring      = hypr_lift(t.focus_ring);
    return t;
}
}  // namespace

const ThemeTokens& builtTokens() {
    static const ThemeTokens dark = makeTokens(false);
    static const ThemeTokens light = makeTokens(true);
    static const ThemeTokens hypr_dark = makeHyprDarkTokens(dark);
    if (is_light())
        return light;
    return is_hypr_dark() ? hypr_dark : dark;
}

}  // namespace

const ThemeTokens& tokens() { return builtTokens(); }

QString css(const QColor& c) {
    if (c.alpha() >= 255)
        return c.name();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(double(c.alpha()) / 255.0, 0, 'f', 2);
}

QColor with_alpha(const QColor& c, int alpha) {
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

void log_theme_tokens() {
    const ThemeTokens& t = tokens();
    auto hex = [](const QColor& c) {
        return c.alpha() >= 255 ? c.name(QColor::HexRgb)
                                : c.name(QColor::HexArgb);
    };
    qWarning().nospace()
        << "[theme] surface=" << hex(t.surface)
        << " surface_low=" << hex(t.surface_low)
        << " surface_raised=" << hex(t.surface_raised)
        << " surface_higher=" << hex(t.surface_higher)
        << " surface_highest=" << hex(t.surface_highest);
    qWarning().nospace()
        << "[theme] border=" << hex(t.border)
        << " border_soft=" << hex(t.border_soft)
        << " border_hi=" << hex(t.border_hi)
        << " ink=" << hex(t.ink)
        << " ink_muted=" << hex(t.ink_muted)
        << " ink_faint=" << hex(t.ink_faint);
    qWarning().nospace()
        << "[theme] accent=" << hex(t.accent)
        << " accent_hover=" << hex(t.accent_hover)
        << " accent_press=" << hex(t.accent_press)
        << " on_accent=" << hex(t.on_accent)
        << " accent_text=" << hex(t.accent_text)
        << " accent_soft=" << hex(t.accent_soft)
        << " accent_line=" << hex(t.accent_line);
    qWarning().nospace()
        << "[theme] playhead=" << hex(t.playhead)
        << " playhead_soft=" << hex(t.playhead_soft)
        << " clip_video=" << hex(t.clip_video)
        << " clip_audio=" << hex(t.clip_audio)
        << " clip_label=" << hex(t.clip_label)
        << " clip_border_video=" << hex(t.clip_border_video)
        << " clip_border_audio=" << hex(t.clip_border_audio)
        << " clip_shadow=" << hex(t.clip_shadow);
    qWarning().nospace()
        << "[theme] danger=" << hex(t.danger)
        << " danger_soft=" << hex(t.danger_soft)
        << " warn=" << hex(t.warn)
        << " state_hover=" << hex(t.state_hover)
        << " state_press=" << hex(t.state_press)
        << " state_selected=" << hex(t.state_selected)
        << " focus_ring=" << hex(t.focus_ring);
}

}  // namespace canvas::gui