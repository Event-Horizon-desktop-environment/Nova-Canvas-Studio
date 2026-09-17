#pragma once

#include <QProxyStyle>

class QPainter;
class QStyleOption;

namespace canvas::gui {

class HorizonStyle final : public QProxyStyle {
public:
    explicit HorizonStyle(QStyle* base);

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                       const QWidget* w) const override;
    void drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                     const QWidget* w) const override;
    int pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const override;
};

}
