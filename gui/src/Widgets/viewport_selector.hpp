#pragma once

#include <QString>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;

namespace canvas::gui {

// Viewport/frame timebase selector for the transport bar. Hand-painted from
// scratch (no QComboBox, no stylesheet subcontrols): a semi-rounded outlined
// button holding the current mode label plus the bundled chevron icon, opening
// a themed popup menu on press. Paints everything itself so no sibling can
// draw over the label or the chevron.
class ViewportSelector final : public QWidget {
    Q_OBJECT
public:
    explicit ViewportSelector(QWidget* parent = nullptr);

    [[nodiscard]] QString current() const { return current_; }

signals:
    void changed(QString item);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void set_current(QString item);

    QString current_{QStringLiteral("Viewport")};
    bool hovered_{false};
};

}  // namespace canvas::gui