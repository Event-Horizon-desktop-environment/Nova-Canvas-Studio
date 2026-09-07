#pragma once

#include <QAbstractItemView>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QSize>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariant>

// Media pool grid. Subclasses QListWidget (IconMode) so the rest of the app can
// keep using addItem()/item()/clear() unchanged, but overlays a Material 3
// "empty state" (title + subtitle + blue accent CTA) whenever the pool has no
// clips, the familiar "no clips in media pool" empty state, so nobody's
class MediaPoolWidget : public QListWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(QWidget* parent = nullptr);

    // Re-evaluates whether to show the empty-state overlay based on the item
    // count. Connected to the model, but public so callers can force a refresh.
    void update_empty_state();

signals:
    // Emitted when the empty-state "Add Clips" button is pressed.
    void importRequested();
    // Emitted when media files are dropped onto the pool from a file browser.
    void filesDropped(const QStringList& paths);
    // Emitted when Delete/Backspace is pressed while pool items are selected.
    void deleteSelectedRequested();
    // Emitted when the Del key is pressed while pool items are selected: like
    // deleteSelectedRequested, but ALSO ripple-deletes the timeline selection.
    void deleteSelectedWithClipsRequested();

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void startDrag(Qt::DropActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setup_empty_state();
    QWidget* empty_state_ = nullptr;
    QPushButton* import_button_ = nullptr;
};
