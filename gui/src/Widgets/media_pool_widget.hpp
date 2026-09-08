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

// Item roles carried by every pool item so the tile delegate can paint a card
// (duration + resolution + type) without touching the model. The media index is
// kept at kPoolMediaIndexRole (Qt::UserRole) for the existing drag/delete paths.
enum MediaPoolRoles {
    kPoolMediaIndexRole = Qt::UserRole,   // index into Project::media
    kPoolIsVideoRole    = Qt::UserRole + 1,
    kPoolResolutionRole = Qt::UserRole + 2,
    kPoolDurationRole   = Qt::UserRole + 3,
};

// Media pool grid. Subclasses QListWidget (IconMode) so the rest of the app can
// keep using addItem()/item()/clear() unchanged, but overlays a themed empty
// state (title + subtitle + accent CTA) whenever the pool has no clips. Each
// item is painted as a card tile by MediaPoolTileDelegate (see the .cpp).
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
