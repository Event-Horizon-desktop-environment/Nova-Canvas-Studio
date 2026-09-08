#include "UX/horizon_style.hpp"

#include "UX/theme.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>

namespace canvas::gui {

namespace {

// Semi-rounded radius shared by buttons (matches the theme's kShapeMedium).
constexpr qreal kButtonRadius = 8.0;

// Paint a flat, semi-rounded surface: base fill with hover/press state
// overlays. No bevels, no inner shadows — a clean Mojo-style flat look. All
// colours come from the active ThemeTokens so the style follows a mode switch.
void paintFlatSurface(QPainter* p, const QRectF& r, qreal radius, const QColor& fill,
                      bool hovered, bool pressed, bool accent, bool checked) {
    const ThemeTokens& t = tokens();
    p->setRenderHint(QPainter::Antialiasing, true);

    QColor base = fill;
    if (accent) {
        base = (pressed | checked) ? t.accent_press : (hovered ? t.accent_hover : t.accent);
    } else if (pressed) {
        base = t.border;
    } else if (hovered) {
        base = t.surface_higher;
    }

    QPainterPath clip;
    clip.addRoundedRect(r, radius, radius);
    p->setClipPath(clip);
    p->fillPath(clip, base);
    p->setClipRect(QRect());
    p->setClipping(false);

    // Optional thin border to separate the surface from its background.
    const QColor border = accent ? t.accent.darker(115) : t.border;
    p->setPen(QPen(border, 1.0));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

}  // namespace

HorizonStyle::HorizonStyle(QStyle* base) : QProxyStyle(base) {}

void HorizonStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                                 const QWidget* w) const {
    switch (pe) {
        case PE_PanelButtonCommand:
        case PE_PanelButtonTool: {
            // Push buttons render as the filled accent surface; non-autoRaised
            // tool buttons (e.g. DIM) render as a flat raised surface. Both are
            // semi-rounded — never square.
            const bool is_cmd = (pe == PE_PanelButtonCommand);
            const bool accent = is_cmd;
            bool hover = opt->state & State_MouseOver;
            bool down = opt->state & State_Sunken && opt->state & State_Enabled;
            bool checked = opt->state & State_On;
            paintFlatSurface(p, QRectF(opt->rect), kButtonRadius,
                             tokens().surface_raised, hover, down, accent, checked);
            return;
        }
        case PE_PanelMenuBar:
        case PE_PanelToolBar:
        case PE_PanelTipLabel: {
            paintFlatSurface(p, QRectF(opt->rect), 10, tokens().surface_raised,
                             false, false, false, false);
            return;
        }
        case PE_FrameGroupBox: {
            QRectF r = opt->rect;
            p->setPen(QPen(tokens().border, 1.0));
            p->setBrush(Qt::NoBrush);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
            return;
        }
        default:
            break;
    }
    QProxyStyle::drawPrimitive(pe, opt, p, w);
}

void HorizonStyle::drawControl(ControlElement ce, const QStyleOption* opt, QPainter* p,
                               const QWidget* w) const {
    switch (ce) {
        case CE_ToolBar: {
            const auto* o = qstyleoption_cast<const QStyleOptionToolBar*>(opt);
            QColor fill = tokens().surface_raised;
            qreal radius = kButtonRadius;  // semi-rounded toolbar ends
            if (o && (o->toolBarArea == Qt::BottomToolBarArea)) {
                // Bottom page bar reads as a recessed full-width well: square ends.
                fill = tokens().surface;
                radius = 0;
            }
            paintFlatSurface(p, QRectF(opt->rect), radius, fill, false, false, false, false);
            return;
        }
        case CE_PushButton:
        case CE_PushButtonBevel: {
            // Glassy bevel drawn in drawPrimitive(PE_PanelButtonCommand); let
            // Fusion handle label + icon on top of it.
            break;
        }
        default:
            break;
    }
    QProxyStyle::drawControl(ce, opt, p, w);
}

int HorizonStyle::pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const {
    if (pm == PM_ButtonMargin) return 8;
    if (pm == PM_MenuBarItemSpacing) return 2;
    return QProxyStyle::pixelMetric(pm, option, widget);
}

}  // namespace canvas::gui