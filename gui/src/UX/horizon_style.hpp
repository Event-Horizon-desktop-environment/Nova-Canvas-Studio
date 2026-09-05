#pragma once

#include <QProxyStyle>

class QPainter;
class QStyleOption;

namespace canvas::gui {

// A Horizon-style QStyle built on Fusion that paints the signature "glassy"
// panels and buttons via QPainter (dark outer separation stroke + inset white
// gradient rim, scaled by control height), matching the Event-Horizon apps.
// The base Fusion style handles everything the glassy treatment does not
// (menus, scrollbars, views, text layout, etc.).
class HorizonStyle final : public QProxyStyle {
public:
    explicit HorizonStyle(QStyle* base);

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                       const QWidget* w) const override;
    void drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                     const QWidget* w) const override;
    int pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const override;
};

}  // namespace canvas::gui
