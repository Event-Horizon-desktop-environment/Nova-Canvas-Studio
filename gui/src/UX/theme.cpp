#include "UX/theme.hpp"

#include "UX/horizon_style.hpp"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QIconEngine>
#include <QMenu>
#include <QObject>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QStyle>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QWidget>

#include <utility>
#include <vector>

namespace canvas::gui {

namespace {

// ---------------------------------------------------------------------------
// Active appearance state. The token sets are built once per mode; switching
// re-applies palette + global stylesheet and runs registered re-apply
// callbacks (per-widget style builders).
// ---------------------------------------------------------------------------
bool g_light = false;
QApplication* g_app = nullptr;
std::vector<std::function<void()>> g_reapply;

ThemeTokens makeTokens(bool light) {
    ThemeTokens t;
    if (light) {
        t.surface        = QColor(0xF5, 0xF6, 0xFA);
        t.surface_low    = QColor(0xFF, 0xFF, 0xFF);
        t.surface_raised = QColor(0xFF, 0xFF, 0xFF);
        t.surface_higher = QColor(0xEB, 0xEF, 0xF6);
        t.surface_highest = QColor(0xFF, 0xFF, 0xFF);
        t.border         = QColor(0xD6, 0xDC, 0xE6);
        t.border_soft    = QColor(0xE3, 0xE8, 0xF0);
        t.border_hi      = QColor(0xEE, 0xF2, 0xF8);
        t.ink            = QColor(0x1B, 0x23, 0x33);
        t.ink_muted      = QColor(0x5B, 0x64, 0x78);
        t.ink_faint      = QColor(0x8B, 0x95, 0xA6);
        t.accent         = QColor(0x10, 0xB9, 0x81);
        t.accent_hover   = QColor(0x0D, 0x9F, 0x6E);
        t.accent_press   = QColor(0x0B, 0x8A, 0x5F);
        t.on_accent      = QColor(0x04, 0x2E, 0x1F);
        t.accent_text    = QColor(0x0E, 0x7B, 0x57);
        t.playhead       = QColor(0x6C, 0x55, 0xFF);
        t.clip_video     = QColor(0xC9, 0xD4, 0xE0);
        t.clip_audio     = QColor(0xD4, 0xDC, 0xC8);
        t.clip_label     = QColor(0xB9, 0xC6, 0xD4);
        t.danger         = QColor(0xE5, 0x48, 0x4D);
        t.warn           = QColor(0xB4, 0x53, 0x09);
        t.focus_ring     = QColor(0x0E, 0x7B, 0x57);
        t.font_ui        = QStringLiteral("Geist");
        t.font_mono      = QStringLiteral("Geist Mono");
    } else {
        t.surface        = QColor(0x11, 0x13, 0x1A);
        t.surface_low    = QColor(0x14, 0x1A, 0x21);
        t.surface_raised = QColor(0x1A, 0x1D, 0x27);
        t.surface_higher = QColor(0x20, 0x24, 0x2F);
        t.surface_highest = QColor(0x27, 0x2C, 0x39);
        t.border         = QColor(0x2A, 0x2F, 0x3C);
        t.border_soft    = QColor(0x23, 0x28, 0x33);
        t.border_hi      = QColor(0x3A, 0x40, 0x4E);
        t.ink            = QColor(0xE8, 0xEA, 0xF0);
        t.ink_muted      = QColor(0x9A, 0xA0, 0xB0);
        t.ink_faint      = QColor(0x5F, 0x65, 0x77);
        t.accent         = QColor(0x10, 0xB9, 0x81);
        t.accent_hover   = QColor(0x34, 0xD3, 0x99);
        t.accent_press   = QColor(0x0E, 0x9C, 0x6F);
        t.on_accent      = QColor(0x05, 0x2E, 0x21);
        t.accent_text    = QColor(0x8F, 0xD9, 0xC0);
        t.playhead       = QColor(0x6C, 0x55, 0xFF);
        t.clip_video     = QColor(0x2A, 0x35, 0x40);
        t.clip_audio     = QColor(0x5A, 0x6B, 0x4A);
        t.clip_label     = QColor(0x3D, 0x5A, 0x73);
        t.danger         = QColor(0xF0, 0x71, 0x7A);
        t.warn           = QColor(0xFB, 0xBF, 0x24);
        t.focus_ring     = QColor(0x8F, 0xD9, 0xC0);
        t.font_ui        = QStringLiteral("Geist");
        t.font_mono      = QStringLiteral("Geist Mono");
    }
    t.accent_soft    = with_alpha(t.accent, 40);
    t.accent_line    = with_alpha(t.accent, 128);
    t.playhead_soft  = with_alpha(t.playhead, 64);
    t.danger_soft    = with_alpha(t.danger, 38);
    t.state_hover    = with_alpha(t.ink, 20);
    t.state_press    = with_alpha(t.ink, 30);
    t.state_selected = with_alpha(t.accent, 51);
    return t;
}

const ThemeTokens& builtTokens() {
    static const ThemeTokens dark = makeTokens(false);
    static const ThemeTokens light = makeTokens(true);
    return g_light ? light : dark;
}

// ---------------------------------------------------------------------------
// Tints for the bundled monochrome SVG icons, mapped per QIcon mode.
// ---------------------------------------------------------------------------
// Reserved accent: SVGs that paint a region in this red (e.g. the horseshoe
// magnet's pole tips) keep it as-authored while the rest of the ink is
// theme-tinted, so two-tone icons stay two-tone across dark and light tokens.
const QColor kReservedAccent(0xE5, 0x48, 0x4D);
class SvgIconEngine : public QIconEngine {
public:
    explicit SvgIconEngine(QString file, QColor normal = QColor(), bool tint = true)
        : file_(std::move(file)), normal_(std::move(normal)), tint_(tint) {}

    QIconEngine* clone() const override { return new SvgIconEngine(file_, normal_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        painter->drawPixmap(rect, pixmap(rect.size(), mode, state));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State) override {
        if (size.isEmpty())
            return QPixmap();  // never paint into a null pixmap (engine == 0)
        int dpr = 1;
        if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
            dpr = int(app->devicePixelRatio());
        }
        const QSize px(size.width() * dpr, size.height() * dpr);

        // Render at 2x the target (supersample) and smoothly downscale, so
        // both vector strokes and any down-converted raster art inside the SVG
        // (e.g. the film-strip thumbnail) stay crisp instead of pixellated.
        constexpr int kSupersample = 2;
        const QSize big(px.width() * kSupersample, px.height() * kSupersample);
        QPixmap hi(big);
        hi.fill(Qt::transparent);
        {
            QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(file_));
            QPainter p(&hi);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(big)));
            p.end();
        }

        QPixmap pm(px);
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(dpr);
        {
            QPainter d(&pm);
            d.setRenderHint(QPainter::SmoothPixmapTransform);
            d.drawPixmap(QRectF(QPointF(0, 0), QSizeF(px)), hi,
                         QRectF(QPointF(0, 0), QSizeF(big)));
            d.end();
        }
        QColor tint = modeColor(mode);
        if (normal_.isValid() && (mode == QIcon::Normal || mode == QIcon::Selected)) {
            tint = normal_;
        }
        if (tint_ && tint.isValid() && tint != QColor(Qt::transparent)) {
            // Tint the glyph with the theme ink by drawing a solid paint layer
            // INTO the rendered shape (SourceIn keeps the source only where the
            // DESTINATION is opaque — i.e. over the glyph, transparent corners
            // stay transparent). The previous order — drawing the glyph into a
            // fully-opaque fill with SourceIn — yielded the glyph's AUTHORED
            // color instead (SourceIn result = source, clipped by destination
            // alpha), so SVG icons authored in black (e.g. Dual-View, blade,
            // snap) rendered as black on the dark theme.
            QPixmap layer(px);
            layer.fill(tint);
            layer.setDevicePixelRatio(dpr);
            QPainter tp(&pm);
            tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tp.drawPixmap(0, 0, layer);
            tp.end();
            // Two-tone support: restore any reserved-accent glyph region (pole
            // tips etc.) from the supersampled render so the accent survives
            // the theme tint AND the smooth downscale.
            QPixmap accent;
            if (extractReservedAccent(hi, &accent)) {
                QPainter ov(&pm);
                ov.setRenderHint(QPainter::SmoothPixmapTransform);
                ov.drawPixmap(QRectF(QPointF(0, 0), QSizeF(px)), accent,
                              QRectF(QPointF(0, 0), QSizeF(big)));
                ov.end();
            }
        }
        return pm;
    }

    // Scans `src` (the supersampled vector render) for reserved-accent pixels
    // and, if any exist, produces a same-size layer holding ONLY those pixels
    // at their authored color. Returns false (leaving `out` untouched) when the
    // icon has no accent, so plain monochrome icons take the fast single-tint
    // path with zero extra cost beyond one scan.
    static bool extractReservedAccent(const QPixmap& src, QPixmap* out) {
        const QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
        QImage sel(img.size(), QImage::Format_ARGB32);
        sel.fill(Qt::transparent);
        const QRgb accent = kReservedAccent.rgba();
        bool any = false;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            QRgb* srow = reinterpret_cast<QRgb*>(sel.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb c = row[x];
                const int r = qRed(c), g = qGreen(c), b = qBlue(c);
                // The reserved accent red: saturated, clearly red-dominant.
                // Anti-aliased blends stay below this bar, so only the solid
                // accent core is preserved and the rest tints with the theme.
                if (r >= 140 && (r - g) > 120 && (r - b) > 120) {
                    srow[x] = qRgba(qRed(accent), qGreen(accent), qBlue(accent), qAlpha(c));
                    any = true;
                }
            }
        }
        if (!any) return false;
        *out = QPixmap::fromImage(sel);
        return true;
    }

private:
    static QColor modeColor(QIcon::Mode mode) {
        const ThemeTokens& t = tokens();
        switch (mode) {
            case QIcon::Normal:
            case QIcon::Selected:
                // Full-strength ink, not ink_muted: toolbar glyphs should read
                // bold and bright against the dark surfaces, not washed out.
                return t.ink;
            case QIcon::Active:
                return t.ink;
            case QIcon::Disabled:
                return t.ink_faint;
        }
        return QColor(Qt::transparent);
    }

    QColor normal_;
    QString file_;
    bool tint_;
};

// Global stylesheet for the flat controls only. Buttons, tool bars and panels
// are intentionally left OUT so HorizonStyle's glassy painting owns them.
QString make_flat_controls_qss() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
      "QWidget { color: %1; }"
      "QToolTip { background-color: %2; color: %1; border: 1px solid %3;"
      " padding: 4px 8px; border-radius: 8px; }"
      "QMenuBar { background: transparent; color: %1; }"
      "QMenuBar::item { background: transparent; border-radius: 8px; padding: 4px 10px; }"
      "QMenuBar::item:selected { background: %4; }"
      "QMenuBar::item:pressed { background: %5; }"
      // Floating glass docks: the body is transparent so the workspace's mint
      // radial glow shows around the floating panels.
      "QDockWidget { background: transparent; color: %1; }"
      // Floating menu card: one raised popup matching the app's panel language —
      // raised surface, lit top hairline catchlight and soft hairline border
      // (all solid colors, no gradient so the banding fix stays intact), generous
      // r radius — with pill-shaped hover items and a mint accent check
      // indicator for checkable actions (right-click / bar menus alike).
      "QMenu { background-color: %16; color: %1;"
      "  border: 1px solid %3; border-top: 1px solid %15;"
      "  border-radius: 14px; padding: 6px; }"
      "QMenu::item { padding: 6px 28px 6px 12px; border-radius: 9px;"
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
      "QComboBox QAbstractItemView { background: %2; border: none;"
      "  selection-background-color: %13; selection-color: %14; outline: none;"
      "  border-radius: 8px; padding: 4px; }"
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
        css(t.border_hi), css(t.surface_raised));
}

void apply_rounded_menu_impl(QMenu* menu) {
    if (!menu) return;
    // Translucent + frameless make the popup's QSS border-radius really clip;
    // without them the native popup window keeps square corners.
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
    menu->setWindowFlag(Qt::FramelessWindowHint, true);
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

QColor with_alpha(const QColor& c, int alpha) {
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

static QPalette makeHorizonPalette();

const ThemeTokens& tokens() { return builtTokens(); }

bool is_light() { return g_light; }

QString css(const QColor& c) {
    if (c.alpha() >= 255)
        return c.name();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(double(c.alpha()) / 255.0, 0, 'f', 2);
}

void register_theme_reapply(std::function<void()> fn) {
    g_reapply.push_back(std::move(fn));
}

void apply_theme_style(QWidget* w, const std::function<QString()>& style) {
    w->setStyleSheet(style());
    register_theme_reapply([w, style] { w->setStyleSheet(style()); });
}

void set_light(bool light) {
    if (g_light == light)
        return;
    g_light = light;
    if (g_app) {
        g_app->setPalette(makeHorizonPalette());
        g_app->setStyleSheet(make_flat_controls_qss());
        // Icon pixmaps are cached per QIcon; drop the whole cache so the next
        // paint re-renders every tinted glyph against the new token set.
        QPixmapCache::clear();
        for (QWidget* w : g_app->allWidgets()) {
            w->update();
            w->style()->unpolish(w);
            w->style()->polish(w);
        }
    }
    for (const auto& fn : g_reapply)
        fn();
}

// ---------------------------------------------------------------------------
// Horizon-based QPalette. Mint accent maps onto Highlight/Accent; surfaces are
// the active token family (dark blue-tinted or clean light).
// ---------------------------------------------------------------------------
static QPalette makeHorizonPalette() {
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

void apply_theme(QApplication& app, bool light) {
    g_app = &app;
    g_light = light;
    QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    app.setStyle(new HorizonStyle(fusion));
    app.setPalette(makeHorizonPalette());
    app.setStyleSheet(make_flat_controls_qss());
    install_popup_rounding(app);
}

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

QString page_switcher_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "background-color: %1; border-top: 1px solid %2;")
        .arg(css(t.surface_raised), css(t.border_soft));
}

QString time_label_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("font-family: %1; color: %2;")
        .arg(t.font_mono, css(t.ink));
}

QString media_pool_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QListWidget { background-color: %1; border: none; }"
        "QListWidget::item { background: transparent; color: %2; padding: 0px;"
        "  border: none; margin: 0px; }"
        "QListWidget::item:hover { background: transparent; }"
        "QListWidget::item:selected { background: transparent; color: %2; }")
        .arg(css(t.surface_low), css(t.ink));
}

// Floating panels: a flat raised card with a hairline border and a generous
// r-xl radius so the panels read as rounded.
QString viewer_frame_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QFrame#viewerFrame { background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 16px; }")
        .arg(css(t.surface_low), css(t.border_soft));
}

QString timeline_frame_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QFrame#timelineFrame { background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 16px; }")
        .arg(css(t.surface), css(t.border_soft));
}

// Flat dock backdrop: no gradient glow, just the workspace surface.
QString dock_glow_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral("QDockWidget { background-color: %1; }")
        .arg(css(t.surface));
}

// Flat card that carries a dock's content: a rounded raised panel with a small
// margin so the workspace surface shows around the card.
QString dock_panel_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#dockGlassCard { background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 16px; margin: 8px 6px; }")
        .arg(css(t.surface_raised), css(t.border_soft));
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
    return QStringLiteral("font-family: %1; font-size: 15px; color: %2; background: transparent;")
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
        " border-radius: 8px; }"
        "QToolButton:hover { background-color: %2; }")
        .arg(css(t.ink), css(t.state_hover));
}

// Header row (the card's top band). Two shapes: OPEN rounds the top corners
// (the body below rounds the bottom); CLOSED rounds all four corners. Scoped
// by objectName so the border never cascades onto the child toggle/reset.
QString inspector_card_header_open_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardHeader {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-top-left-radius: 16px; border-top-right-radius: 16px; }")
        .arg(css(t.surface_highest), css(t.border_soft));
}

QString inspector_card_header_closed_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardHeader {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 16px; }")
        .arg(css(t.surface_highest), css(t.border_soft));
}

QString inspector_card_body_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget#inspectorCardBody { background-color: %1;"
        " border-bottom-left-radius: 16px; border-bottom-right-radius: 16px;"
        " border-left: 1px solid %2; border-right: 1px solid %2;"
        " border-bottom: 1px solid %2; }")
        .arg(css(t.surface_low), css(t.border_soft));
}

// Segmented pill in a recessed track (the page/deliver switcher bar): inactive
// members float transparently on the recessed well; the active member is a
// raised glass segment with a lit top edge and accent-tinted text.
QString page_pill_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; padding: 5px 10px; border-radius: 10px;"
        " background: transparent; border: 1px solid transparent; }"
        "QToolButton:hover:!checked { color: %2; background-color: %3; }"
        "QToolButton:pressed { background-color: %4; }"
        "QToolButton:checked { color: %2; background-color: %5;"
        " border: 1px solid %6;"
        " font-weight: 600; }")
        .arg(css(t.ink_muted), css(t.ink), css(t.state_hover),
             css(t.state_press), css(t.surface_highest), css(t.border));
}

// Inspector mode tabs: a recessed semi-rounded segmented track (the pill row's
// container) with individual pills that read as one segmented control. The
// active segment is a raised glass pill with accent-tinted text.
QString inspector_tab_track_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QWidget { background-color: %1;"
        " border: 1px solid %2; border-radius: 14px; }")
        .arg(css(t.surface_low), css(t.border_soft));
}

QString inspector_tab_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; background: transparent; border: none;"
        " border-radius: 11px; padding: 5px 4px; font-weight: 500; }"
        "QToolButton:hover:!checked { color: %2; background-color: %3; }"
        "QToolButton:checked { color: %4; background-color: %5;"
        " border: 1px solid %6; font-weight: 600; }")
        .arg(css(t.ink_muted), css(t.ink), css(t.state_hover),
             css(t.accent_text), css(t.surface_highest), css(t.accent_line));
}

// Left "Media Pool / Sync Bin / ..." tab strip: the pane is transparent so the
// floating glass card behind it shows through; tabs are raised glass pills (the
// page_pill/inspector_tab idiom) instead of the global underline style.
QString left_tab_strip_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QTabWidget#leftTabStrip::pane{background:transparent;border:none;}"
        "QTabBar::tab{background:transparent;color:%1;padding:6px 12px;"
        "  border:none;border-radius:10px;font-weight:500;margin:2px 1px;}"
        "QTabBar::tab:hover{color:%2;background-color:%3;}"
        "QTabBar::tab:selected{color:%4;background-color:%5;"
        "  border:1px solid %7;font-weight:600;}"
        "QTabBar::tab:selected:hover{color:%4;}"
        "QTabBar QToolButton{background:transparent;border:none;border-radius:8px;}"
        "QTabBar QToolButton:hover{background:%3;}")
        .arg(css(t.ink_muted), css(t.ink), css(t.state_hover),
             css(t.accent_text), css(t.surface_highest), css(t.surface_raised),
             css(t.accent_line), css(t.border_hi));
}

// Flat icon toolbar buttons — every icon button now has a subtle raised surface
// so the whole chrome feels physical, not invisible-until-hovered.
QString flat_tool_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: %1; border: 1px solid %2; border-radius: 6px;"
        " padding: 4px; }"
        "QToolButton:hover { background: %3; border-color: %4; }"
        "QToolButton:pressed { background: %4; }"
        "QToolButton:checked { background: %5;"
        " border: 1px solid %6; }")
        .arg(css(t.surface_raised), css(t.border_soft), css(t.surface_higher),
             css(t.border), css(t.state_selected), css(t.accent_line));
}

// "Bracket-pill" edit-tool cluster (select/trim/blade/mode): raised members
// with a cohesive border, active member shows a soft accent fill.
QString tool_cluster_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { background: %1; border: 1px solid %2; border-radius: 6px;"
        " padding: 4px; }"
        "QToolButton:hover { background: %3; border-color: %4; }"
        "QToolButton:checked { background: %5;"
        " border: 1px solid %6; border-radius: 6px; }")
        .arg(css(t.surface_raised), css(t.border_soft), css(t.surface_higher),
             css(t.border), css(t.state_selected), css(t.accent_line));
}

// Semi-rounded outlined button (DIM) — raised surface with a colored border.
QString outline_pill_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QToolButton { color: %1; background: %2; border: 1px solid %3;"
        " border-radius: 8px; padding: 3px 12px; font-size: 10px; }"
        "QToolButton:hover { border-color: %4; color: %5; background: %6; }"
        "QToolButton:checked { border-color: %7; color: %8; background: %9; }")
        .arg(css(t.ink), css(t.surface_raised), css(t.border),
             css(t.ink_muted), css(t.ink), css(t.surface_higher),
             css(t.danger), css(t.danger), css(t.danger_soft));
}

// Semi-rounded sliders: pill groove, round handle — never square.
QString slider_style() {
    const ThemeTokens& t = tokens();
    return QStringLiteral(
        "QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; background: %3;"
        " border: none; border-radius: 8px; }"
        "QSlider::handle:horizontal:hover { background: %4; }")
        .arg(css(t.border), css(t.accent), css(t.ink), css(t.surface_highest));
}

// Circular mint play button: a round accent disc — flat, clean, no bevel.
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

QIcon icon(const char* name) { return QIcon(new SvgIconEngine(QString::fromLatin1(name))); }

QIcon icon(const char* name, const QColor& normal) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), normal));
}

QIcon raw_icon(const char* name) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), QColor(), /*tint=*/false));
}

}  // namespace canvas::gui