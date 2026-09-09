#pragma once

// Node graph canvas (design spec §Node Graph): a lightweight 2D node canvas
// with a serial chain, connections, canvas-level context menu, wheel zoom and
// middle-drag pan. Milestone 0 scaffold — nodes exist as interactive objects on
// the canvas but do not yet own grade math.

#include <QGraphicsView>
#include <QPoint>
#include <QString>
#include <QVector>

class QGraphicsScene;
class QGraphicsPathItem;

namespace canvas::gui {

class NodeGraphCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit NodeGraphCanvas(QWidget* parent = nullptr);

    void add_node(int index, const QString& kind, const QString& label);
    void clear_nodes();
    [[nodiscard]] int node_count() const { return node_items_.size(); }

signals:
    void node_activated(int index);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void fit_to_content();
    void rebuild_connections();
    void layout_nodes();
    void open_context_menu(const QPoint& pos);

    QGraphicsScene* scene_ = nullptr;
    QVector<QGraphicsItem*> node_items_;
    QVector<QGraphicsPathItem*> connections_;
    bool panning_ = false;
    QPoint last_pan_pos_;
};

}  // namespace canvas::gui