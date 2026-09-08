#include "Widgets/viewport_selector.hpp"

#include <QEnterEvent>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

#include "UX/theme.hpp"

namespace canvas::gui {

namespace {

constexpr int kW = 92;
constexpr int kH = 24;
constexpr int kRadius = 8;
constexpr int kTextLeft = 10;
constexpr int kChevronRight = 4;

}  // namespace

ViewportSelector::ViewportSelector(QWidget* parent) : QWidget(parent) {
    setFixedSize(kW, kH);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Viewport / timebase selector"));
    setMouseTracking(true);
    QFont f = font();
    // Point size (not pixel size) so the label scales with the display's DPI.
    f.setPointSizeF(7.5);
    setFont(f);
}

void ViewportSelector::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(0.5, 0.5, kW - 1.0, kH - 1.0);
    const ThemeTokens& t = tokens();
    p.setPen(QPen(hovered_ ? t.border : t.border_soft, 2.0));
    p.setBrush(hovered_ ? t.surface_higher : t.surface_raised);
    p.drawRoundedRect(box, kRadius, kRadius);

    p.setPen(t.ink);
    const QFontMetrics fm(font());
    const QRect text_rect(kTextLeft, 0, kW - kTextLeft - 20, kH);
    p.setFont(font());
    p.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, current_);

    const qreal dpr = devicePixelRatioF();
    QPixmap chevron = icon("chevron_down").pixmap(QSize(16, 16) * dpr);
    chevron.setDevicePixelRatio(dpr);
    const QRect chevron_rect(kW - kChevronRight - 16, (kH - 16) / 2, 16, 16);
    p.drawPixmap(chevron_rect, chevron);
}

void ViewportSelector::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    event->accept();

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction* viewport = menu.addAction(tr("Viewport"));
    QAction* frame = menu.addAction(tr("Frame"));
    viewport->setCheckable(true);
    frame->setCheckable(true);
    if (current_ == frame->text()) {
        frame->setChecked(true);
    } else {
        viewport->setChecked(true);
    }

    const QAction* chosen = menu.exec(mapToGlobal(QPoint(0, kH + 2)));
    if (chosen && chosen->isCheckable()) {
        set_current(chosen->text());
    }
}

void ViewportSelector::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void ViewportSelector::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void ViewportSelector::set_current(QString item) {
    if (item == current_) return;
    current_ = std::move(item);
    update();
    emit changed(current_);
}

}  // namespace canvas::gui