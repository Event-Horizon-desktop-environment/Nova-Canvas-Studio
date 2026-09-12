#pragma once

// Node graph canvas (design spec §Node Graph): a lightweight 2D node canvas
// with a serial chain, connections, canvas-level context menu, wheel zoom and
// middle-drag pan. Milestone 0 scaffold — nodes exist as interactive objects on
// the canvas but do not yet own grade math. `load_graph` renders a clip's
// actual headless GradeGraph as a serial chain so the node tree is visible in
// the color page.

#include "canvas/core/grade_graph/graph.hpp"

#include <QGraphicsView>
#include <QPoint>
#include <QString>
#include <QVector>

class QGraphicsScene;
class QGraphicsPathItem;
class QKeyEvent;

namespace canvas::gui {

class NodeGraphCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit NodeGraphCanvas(QWidget* parent = nullptr);

    void add_node(int index, const QString& kind, const QString& label);
    void clear_nodes();
    [[nodiscard]] int node_count() const { return node_items_.size(); }
    // Renders a clip's grade tree as the canvas' serial chain (node per tree
    // node, labels carry the corrector mode). Empty graph clears the canvas.
    void load_graph(const canvas::core::grade_graph::GradeGraph& graph);

signals:
    void node_activated(int index);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void fit_to_content();
    void rebuild_connections();
    void layout_nodes();
    void open_context_menu(const QPoint& pos);
    void delete_selected_nodes();

    QGraphicsScene* scene_ = nullptr;
    QVector<QGraphicsItem*> node_items_;
    QVector<QGraphicsPathItem*> connections_;
    bool panning_ = false;
    QPoint last_pan_pos_;
};

}  // namespace canvas::gui