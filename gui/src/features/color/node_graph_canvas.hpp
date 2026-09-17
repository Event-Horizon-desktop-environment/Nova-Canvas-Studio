#pragma once

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

}
