#include "features/color/node_graph_canvas.hpp"

#include <QApplication>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHeaderView>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>

#include "UX/theme.hpp"

namespace canvas::gui {

namespace {

// One serial-chain node: rounded body, a placeholder thumbnail well, kind +
// label text, and triangular in/out ports on the left/right edges.
class NodeGraphItem final : public QGraphicsItem {
public:
    NodeGraphItem(int index, const QString& kind, const QString& label)
        : index_(index), kind_(kind), label_(label) {
        setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setCursor(Qt::PointingHandCursor);
        setZValue(2.0);
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return QRectF(0.0, 0.0, 156.0, 52.0);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const ThemeTokens& t = tokens();
        const QRectF body = boundingRect();

        QLinearGradient grad(body.topLeft(), body.bottomLeft());
        grad.setColorAt(0.0, t.surface_raised);
        grad.setColorAt(1.0, t.surface_low);
        painter->setPen(QPen(isSelected() ? t.accent : t.border, isSelected() ? 1.8 : 1.0));
        painter->setBrush(grad);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->drawRoundedRect(body, 8.0, 8.0);

        // Thumbnail well.
        const QRectF well(body.left() + 7.0, body.center().y() - 12.0, 24.0, 24.0);
        QLinearGradient wg(well.topLeft(), well.bottomRight());
        wg.setColorAt(0.0, tint_for_kind().lighter(130));
        wg.setColorAt(1.0, tint_for_kind().darker(150));
        painter->setPen(QPen(t.border, 1.0));
        painter->setBrush(wg);
        painter->drawRoundedRect(well, 4.0, 4.0);

        // Text.
        painter->setPen(isSelected() ? t.ink : with_alpha(t.ink, 210));
        painter->setFont(QFont(QStringLiteral("sans-serif"), 10, QFont::Bold));
        painter->drawText(QRectF(well.right() + 8.0, body.top() + 7.0,
                                 body.width() - well.right() - 16.0, 15.0),
                          Qt::AlignLeft | Qt::AlignVCenter, kind_);
        painter->setPen(with_alpha(t.ink, 130));
        const QString meta = label_.isEmpty() ? QStringLiteral("Node %1").arg(index_ + 1)
                                              : label_;
        painter->drawText(QRectF(well.right() + 8.0, body.top() + 24.0,
                                 body.width() - well.right() - 16.0, 13.0),
                          Qt::AlignLeft | Qt::AlignVCenter, meta);

        // Ports.
        const QColor port = isSelected() ? t.accent : t.ink_muted;
        painter->setBrush(port);
        painter->setPen(Qt::NoPen);
        for (bool out : {false, true}) {
            const qreal x = out ? body.right() : body.left();
            const QPointF c(x, body.center().y());
            QPolygonF tri;
            if (out) {
                tri << QPointF(c.x() - 5.0, c.y() - 6.0) << QPointF(c.x() - 5.0, c.y() + 6.0)
                    << QPointF(c.x() + 2.0, c.y());
            } else {
                tri << QPointF(c.x() + 5.0, c.y() - 6.0) << QPointF(c.x() + 5.0, c.y() + 6.0)
                    << QPointF(c.x() - 2.0, c.y());
            }
            painter->drawPolygon(tri);
        }
    }

    [[nodiscard]] int index() const { return index_; }
    [[nodiscard]] QString kind() const { return kind_; }

private:
    [[nodiscard]] QColor tint_for_kind() const {
        if (kind_ == "Contrast") return QColor(0x4F, 0x86, 0xD9);
        if (kind_ == "Balance") return QColor(0xE0, 0x9A, 0x4F);
        if (kind_ == "Look") return QColor(0x9C, 0x5B, 0xB5);
        return QColor(0x7B, 0xC9, 0x50);
    }

    int index_ = -1;
    QString kind_;
    QString label_;
};

}  // namespace

NodeGraphCanvas::NodeGraphCanvas(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setDragMode(QGraphicsView::RubberBandDrag);
    setRenderHint(QPainter::Antialiasing);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    apply_theme_style(this, [] {
        return QStringLiteral("QGraphicsView { background-color: %1; border: 1px solid %2;"
                              " border-radius: 8px; }")
            .arg(css(tokens().surface), css(tokens().border));
    });
    scene_->setBackgroundBrush(tokens().surface);
    scene_->setItemIndexMethod(QGraphicsScene::NoIndex);

    setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(this, &QGraphicsView::customContextMenuRequested, this,
                     [this](const QPoint& pos) { open_context_menu(pos); });

    // Starter serial chain.
    add_node(0, "Balance", "Scene");
    add_node(1, "Contrast", "Scene");
    add_node(2, "Look", "Film");
}

void NodeGraphCanvas::add_node(int index, const QString& kind, const QString& label) {
    auto* item = new NodeGraphItem(index, kind, label);
    scene_->addItem(item);
    node_items_.append(item);
    layout_nodes();
    rebuild_connections();
}

void NodeGraphCanvas::clear_nodes() {
    for (auto* c : connections_) scene_->removeItem(c);
    connections_.clear();
    for (auto* n : node_items_) scene_->removeItem(n);
    node_items_.clear();
}

void NodeGraphCanvas::layout_nodes() {
    for (int i = 0; i < node_items_.size(); ++i) {
        node_items_[i]->setPos(40.0 + i * 188.0, 30.0 + (i % 2) * 66.0);
    }
}

void NodeGraphCanvas::rebuild_connections() {
    for (auto* c : connections_) scene_->removeItem(c);
    connections_.clear();
    for (int i = 1; i < node_items_.size(); ++i) {
        QGraphicsItem* from = node_items_[i - 1];
        QGraphicsItem* to = node_items_[i];
        const QPointF out(from->pos().x() + from->boundingRect().width(),
                          from->pos().y() + from->boundingRect().height() / 2.0);
        const QPointF in(to->pos().x(), to->pos().y() + to->boundingRect().height() / 2.0);
        QPainterPath path;
        path.moveTo(out);
        path.cubicTo(QPointF(out.x() + 55.0, out.y()),
                     QPointF(in.x() - 55.0, in.y()), in);
        auto* conn = new QGraphicsPathItem(path);
        conn->setPen(QPen(with_alpha(tokens().accent, 190), 2.0));
        conn->setBrush(Qt::NoBrush);
        conn->setZValue(1.0);
        scene_->addItem(conn);
        connections_.append(conn);
    }
}

void NodeGraphCanvas::open_context_menu(const QPoint& pos) {
    QMenu menu(this);
    const auto add_kind = [this, &menu](const QString& kind, const QString& label) {
        const auto* act = menu.addAction(kind);
        QObject::connect(act, &QAction::triggered, this,
                         [this, kind, label] { add_node(int(node_items_.size()), kind, label); });
        return act;
    };
    add_kind(QStringLiteral("Balance"), QStringLiteral("Scene"));
    add_kind(QStringLiteral("Contrast"), QStringLiteral("Scene"));
    add_kind(QStringLiteral("Look"), QStringLiteral("Film"));
    add_kind(QStringLiteral("Highlight"), QStringLiteral("Node %1").arg(node_items_.size() + 1));
    add_kind(QStringLiteral("Noise"), QStringLiteral("Node %1").arg(node_items_.size() + 1));
    menu.addSeparator();
    QAction* fit = menu.addAction(QStringLiteral("Fit nodes to view"));
    QObject::connect(fit, &QAction::triggered, this, &NodeGraphCanvas::fit_to_content);
    menu.exec(mapToGlobal(pos));
}

void NodeGraphCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        last_pan_pos_ = event->globalPosition().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        // Clicking a node bubbles index-> handler; clicking the canvas clears.
        NodeGraphItem* item = dynamic_cast<NodeGraphItem*>(itemAt(event->position().toPoint()));
        if (item) {
            emit node_activated(item->index());
            scene_->clearSelection();
            item->setSelected(true);
            event->accept();
            return;
        }
        scene_->clearSelection();
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPoint delta = event->globalPosition().toPoint() - last_pan_pos_;
        last_pan_pos_ = event->globalPosition().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void NodeGraphCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void NodeGraphCanvas::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(1.15, event->angleDelta().y() / 120.0);
    scale(factor, factor);
    event->accept();
}

void NodeGraphCanvas::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    fit_to_content();
}

void NodeGraphCanvas::fit_to_content() {
    if (node_items_.isEmpty()) return;
    fitInView(scene_->itemsBoundingRect().adjusted(-70.0, -60.0, 70.0, 60.0),
              Qt::KeepAspectRatio);
}

}  // namespace canvas::gui