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

const QColor kReservedAccent(0xE5, 0x48, 0x4D);
class SvgIconEngine : public QIconEngine {
public:
    explicit SvgIconEngine(QString file, QColor normal = QColor(), bool tint = true)
        : normal_(std::move(normal)), file_(std::move(file)), tint_(tint) {}

    QIconEngine* clone() const override { return new SvgIconEngine(file_, normal_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        painter->drawPixmap(rect, pixmap(rect.size(), mode, state));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State) override {
        if (size.isEmpty())
            return QPixmap();
        int dpr = 1;
        if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
            dpr = int(app->devicePixelRatio());
        }
        const QSize px(size.width() * dpr, size.height() * dpr);

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
            QPixmap layer(px);
            layer.fill(tint);
            layer.setDevicePixelRatio(dpr);
            QPainter tp(&pm);
            tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tp.drawPixmap(0, 0, layer);
            tp.end();
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
                return t.icon;
            case QIcon::Active:
                return t.icon;
            case QIcon::Disabled:
                return t.ink_faint;
        }
        return QColor(Qt::transparent);
    }

    QColor normal_;
    QString file_;
    bool tint_;
};

}

QIcon icon(const char* name) { return QIcon(new SvgIconEngine(QString::fromLatin1(name))); }

QIcon icon(const char* name, const QColor& normal) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), normal));
}

QIcon raw_icon(const char* name) {
    return QIcon(new SvgIconEngine(QString::fromLatin1(name), QColor(), false));
}

}
