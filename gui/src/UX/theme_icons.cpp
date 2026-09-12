#include "UX/theme_icons.hpp"

#include "UX/theme_tokens.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <utility>

namespace canvas::gui {

namespace {

// Tints for the bundled monochrome SVG icons, mapped per QIcon mode.
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

}  // namespace

QIcon icon(const char* name) { return QIcon(new SvgIconEngine(QString::fromLatin1(name))); }

QIcon icon(const char* name, const QColor& normal) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), normal));
}

QIcon raw_icon(const char* name) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), QColor(), /*tint=*/false));
}

}  // namespace canvas::gui