#include "UX/theme_tokens.hpp"

#include "UX/theme_state.hpp"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
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
        t.icon           = QColor(0x1C, 0x1C, 0x1E);  // same as ink by default
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
        t.icon           = QColor(0xED, 0xED, 0xF0);  // same as ink by default
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
    t.icon            = hypr_lift(t.icon);
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

// User overrides (Settings > Theme). Invalid = designed value kept. Applied on
// top of the mode's base token set, so they survive mode toggles (the user's
// accent rides along into Light/Dark/HyprDark rather than being reset).
std::array<std::optional<QColor>, kThemeTokenFieldCount> g_overrides;

// Stable names shared by the QSettings keys and the shareable theme JSON.
const char* kThemeFieldNames[] = {
    "surface", "surface_low", "surface_raised", "surface_higher",
    "surface_highest", "border", "border_soft", "ink", "ink_muted",
    "ink_faint", "icon", "accent", "playhead", "clip_video", "clip_audio",
    "clip_label", "clip_border_video", "clip_border_audio", "danger", "warn",
};
static_assert(std::size(kThemeFieldNames) == kThemeTokenFieldCount);

ThemeTokenField field_from_name(const char* name) {
    if (!name) return ThemeTokenField::COUNT_;
    for (int i = 0; i < kThemeTokenFieldCount; ++i) {
        if (std::strcmp(name, kThemeFieldNames[i]) == 0)
            return static_cast<ThemeTokenField>(i);
    }
    return ThemeTokenField::COUNT_;
}

QColor field_of(const ThemeTokens& t, ThemeTokenField f) {
    switch (f) {
    case ThemeTokenField::Surface: return t.surface;
    case ThemeTokenField::SurfaceLow: return t.surface_low;
    case ThemeTokenField::SurfaceRaised: return t.surface_raised;
    case ThemeTokenField::SurfaceHigher: return t.surface_higher;
    case ThemeTokenField::SurfaceHighest: return t.surface_highest;
    case ThemeTokenField::Border: return t.border;
    case ThemeTokenField::BorderSoft: return t.border_soft;
    case ThemeTokenField::Ink: return t.ink;
    case ThemeTokenField::InkMuted: return t.ink_muted;
    case ThemeTokenField::InkFaint: return t.ink_faint;
    case ThemeTokenField::Icon: return t.icon;
    case ThemeTokenField::Accent: return t.accent;
    case ThemeTokenField::Playhead: return t.playhead;
    case ThemeTokenField::ClipVideo: return t.clip_video;
    case ThemeTokenField::ClipAudio: return t.clip_audio;
    case ThemeTokenField::ClipLabel: return t.clip_label;
    case ThemeTokenField::ClipBorderVideo: return t.clip_border_video;
    case ThemeTokenField::ClipBorderAudio: return t.clip_border_audio;
    case ThemeTokenField::Danger: return t.danger;
    case ThemeTokenField::Warn: return t.warn;
    case ThemeTokenField::COUNT_: return QColor();
    }
    return QColor();
}

void set_field(ThemeTokens& t, ThemeTokenField f, const QColor& c) {
    switch (f) {
    case ThemeTokenField::Surface: t.surface = c; break;
    case ThemeTokenField::SurfaceLow: t.surface_low = c; break;
    case ThemeTokenField::SurfaceRaised: t.surface_raised = c; break;
    case ThemeTokenField::SurfaceHigher: t.surface_higher = c; break;
    case ThemeTokenField::SurfaceHighest: t.surface_highest = c; break;
    case ThemeTokenField::Border: t.border = c; break;
    case ThemeTokenField::BorderSoft: t.border_soft = c; break;
    case ThemeTokenField::Ink: t.ink = c; break;
    case ThemeTokenField::InkMuted: t.ink_muted = c; break;
    case ThemeTokenField::InkFaint: t.ink_faint = c; break;
    case ThemeTokenField::Icon: t.icon = c; break;
    case ThemeTokenField::Accent: t.accent = c; break;
    case ThemeTokenField::Playhead: t.playhead = c; break;
    case ThemeTokenField::ClipVideo: t.clip_video = c; break;
    case ThemeTokenField::ClipAudio: t.clip_audio = c; break;
    case ThemeTokenField::ClipLabel: t.clip_label = c; break;
    case ThemeTokenField::ClipBorderVideo: t.clip_border_video = c; break;
    case ThemeTokenField::ClipBorderAudio: t.clip_border_audio = c; break;
    case ThemeTokenField::Danger: t.danger = c; break;
    case ThemeTokenField::Warn: t.warn = c; break;
    case ThemeTokenField::COUNT_: break;
    }
}

// Derived-family law: recoloring a base field should recolor every member
// derived from it, preserving the member's designed offset/alpha over the base
// (see the accent-family note in the header).
QColor offset(const QColor& base, const QColor& design, const QColor& override) {
    if (!design.isValid()) return override;
    const int dR = design.red() - base.red();
    const int dG = design.green() - base.green();
    const int dB = design.blue() - base.blue();
    auto c = [&](int v) { return qBound(0, v, 255); };
    return QColor(c(override.red() + dR), c(override.green() + dG),
                  c(override.blue() + dB), design.alpha());
}

QString theme_token_hex(const QColor& c) {
    if (!c.isValid()) return QStringLiteral("<invalid>");
    return c.alpha() >= 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

ThemeTokens apply_overrides(const ThemeTokens& base) {
    ThemeTokens t = base;
    for (int i = 0; i < kThemeTokenFieldCount; ++i) {
        const auto f = static_cast<ThemeTokenField>(i);
        if (g_overrides[static_cast<size_t>(f)])
            set_field(t, f, *g_overrides[static_cast<size_t>(f)]);
    }
    // Accent recolors its direct BUTTON states (hover/press/on-accent) so a
    // command button always reads as one family. Everything wider — selection
    // fills (state_selected), accent-tinted text, soft/line fills and the
    // focus ring — stays on its designed color, so a custom accent never flushes
    // the whole chrome into one tint.
    if (const auto& a = g_overrides[static_cast<size_t>(ThemeTokenField::Accent)]) {
        t.accent_hover = offset(base.accent, base.accent_hover, *a);
        t.accent_press = offset(base.accent, base.accent_press, *a);
        t.on_accent    = offset(base.accent, base.on_accent, *a);
    }
    if (const auto& p = g_overrides[static_cast<size_t>(ThemeTokenField::Playhead)]) {
        t.playhead      = *p;
        t.playhead_soft = with_alpha(*p, base.playhead_soft.alpha());
    }
    if (const auto& i = g_overrides[static_cast<size_t>(ThemeTokenField::Ink)]) {
        // Ink drives the M3 state layers.
        t.state_hover = with_alpha(*i, base.state_hover.alpha());
        t.state_press = with_alpha(*i, base.state_press.alpha());
    }
    if (const auto& d = g_overrides[static_cast<size_t>(ThemeTokenField::Danger)]) {
        t.danger      = *d;
        t.danger_soft = with_alpha(*d, base.danger_soft.alpha());
    }
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Border)])
        t.border_hi = t.border;

    // Audit: what the derived-family law recomputed this pass (only for fields
    // with an active override). Lets a person see exactly how an accent/probe
    // change ripples into the chrome, one line per effective family.
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Accent)]) {
        qWarning().nospace()
            << "[theme] derived from accent: accent_hover="
            << theme_token_hex(t.accent_hover)
            << " accent_press=" << theme_token_hex(t.accent_press)
            << " on_accent=" << theme_token_hex(t.on_accent)
            << " (wider chrome keeps designed colors)";
    }
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Playhead)]) {
        qWarning().nospace()
            << "[theme] derived from playhead: playhead_soft="
            << theme_token_hex(t.playhead_soft);
    }
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Ink)]) {
        qWarning().nospace()
            << "[theme] derived from ink: state_hover="
            << theme_token_hex(t.state_hover)
            << " state_press=" << theme_token_hex(t.state_press);
    }
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Danger)]) {
        qWarning().nospace()
            << "[theme] derived from danger: danger_soft="
            << theme_token_hex(t.danger_soft);
    }
    if (g_overrides[static_cast<size_t>(ThemeTokenField::Border)]) {
        qWarning().nospace()
            << "[theme] derived from border: border_hi="
            << theme_token_hex(t.border_hi);
    }
    return t;
}

}  // namespace

const char* theme_token_field_name(ThemeTokenField field) {
    const size_t i = static_cast<size_t>(field);
    return i < kThemeTokenFieldCount ? kThemeFieldNames[i] : "";
}

QString theme_setting_key(ThemeTokenField field) {
    return QStringLiteral("settings/theme/")
        + QLatin1String(theme_token_field_name(field));
}

QString theme_color_string(const QColor& c) {
    if (!c.isValid()) return {};
    return c.alpha() >= 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb);
}

void set_token_override(ThemeTokenField field, const QColor& color) {
    const size_t i = static_cast<size_t>(field);
    if (i >= kThemeTokenFieldCount) return;
    g_overrides[i] = color.isValid()
        ? std::optional<QColor>(color)
        : std::nullopt;
    // Audit trail: every override mutation passes through here (dialog pick /
    // reset / reset-all / import / boot restore).
    if (color.isValid()) {
        qWarning().nospace()
            << "[theme] override " << theme_token_field_name(field)
            << " = " << theme_token_hex(color);
    } else {
        qWarning().nospace()
            << "[theme] override " << theme_token_field_name(field)
            << " cleared (designed value restored)";
    }
}

QColor token_override(ThemeTokenField field) {
    const size_t i = static_cast<size_t>(field);
    if (i >= kThemeTokenFieldCount) return {};
    return g_overrides[i] ? *g_overrides[i] : QColor();
}

QColor designed_token_value(ThemeTokenField field) {
    return field_of(builtTokens(), field);
}

const ThemeTokens& tokens() {
    // Compose the active token set from the mode base + any user overrides on
    // every cache build. Recompute when the mode's base object changes address
    // (builtTokens returns one of three static sets) or when any override
    // changes.
    static const ThemeTokens* cached_base = nullptr;
    static std::array<std::optional<QColor>, kThemeTokenFieldCount> cached_overrides;
    static ThemeTokens cached;
    const ThemeTokens& base = builtTokens();
    if (std::addressof(base) != cached_base
        || g_overrides != cached_overrides) {
        cached = apply_overrides(base);
        cached_base = std::addressof(base);
        cached_overrides = g_overrides;
    }
    return cached;
}

bool export_theme_file(const QString& path, const QString& name) {
    QJsonObject overrides;
    int n = 0;
    for (int i = 0; i < kThemeTokenFieldCount; ++i) {
        const auto f = static_cast<ThemeTokenField>(i);
        const QColor c = token_override(f);
        if (c.isValid()) {
            overrides.insert(QLatin1String(theme_token_field_name(f)),
                             theme_color_string(c));
            ++n;
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("nova-canvas-theme"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("light"), is_light());
    root.insert(QStringLiteral("overrides"), overrides);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().nospace()
            << "[theme] export FAILED " << path << " (cannot open for write)";
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    qWarning().nospace()
        << "[theme] export OK " << path << " - " << n
        << " override(s), name='" << name << "'";
    return true;
}

bool import_theme_file(const QString& path, QString* name_out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning().nospace() << "[theme] import FAILED " << path
                             << " (cannot open)";
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning().nospace()
            << "[theme] import FAILED " << path
            << " (JSON parse: " << err.errorString() << ")";
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString()
            != QStringLiteral("nova-canvas-theme")) {
        qWarning().nospace()
            << "[theme] import FAILED " << path
            << " (not a nova-canvas-theme file)";
        return false;
    }
    const QJsonObject overrides = root.value(QStringLiteral("overrides")).toObject();
    int applied = 0;
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        const ThemeTokenField field = field_from_name(it.key().toUtf8().constData());
        if (field == ThemeTokenField::COUNT_) {
            qWarning().nospace()
                << "[theme] import " << path << " skips unknown field '"
                << it.key() << "'";
            continue;
        }
        const QColor c(it.value().toString());
        if (c.isValid()) {
            set_token_override(field, c);
            ++applied;
        } else {
            qWarning().nospace()
                << "[theme] import " << path << " skips invalid color on "
                << it.key() << " (" << it.value().toString() << ")";
        }
    }
    if (name_out && root.value(QStringLiteral("name")).isString())
        *name_out = root.value(QStringLiteral("name")).toString();
    qWarning().nospace()
        << "[theme] import OK " << path << " - " << applied
        << " override(s) applied";
    return true;
}

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
        << " ink_faint=" << hex(t.ink_faint)
        << " icon=" << hex(t.icon);
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