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
#include <QMouseEvent>
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
    kPoolHasAudioRole   = Qt::UserRole + 4,  // media carries an audio stream (=> hybrid tile)
    // Generated audio-spectrum strip for video+audio tiles; the video frame
    // stays in the DecorationRole icon so the tile can show both at once.
    kPoolWaveformImageRole = Qt::UserRole + 5,
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

    // Hover-scrub playhead state, consumed by the tile delegate (paints the
    // accent playhead across the hovered well) and the Dual-Viewer wiring
    // (clipScrubbed drives the source preview). index is the pool row; -1
    // means no scrub is active.
    [[nodiscard]] int scrub_index() const { return scrub_index_; }
    [[nodiscard]] double scrub_fraction() const { return scrub_fraction_; }

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
    // Hover-scrub skim: emitted on every mouse move over a tile (unpressed)
    // with the pointer's x as a 0..1 fraction of the tile width. Drives the
    // Source Viewer in Dual-View mode (Resolve-style Live Media Preview).
    void clipScrubbed(int media_index, double fraction);
    // Emitted once on leave (with the final hovered index) so the wiring can
    // keep showing the last skimmed frame instead of resetting to tile 0.
    void clipScrubEnded(int media_index);

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void startDrag(Qt::DropActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setup_empty_state();
    void update_scrub_from_mouse(const QPoint& viewport_pos);
    QWidget* empty_state_ = nullptr;
    QPushButton* import_button_ = nullptr;
    // Current hover-scrub state (row + fraction of the tile width).
    int scrub_index_ = -1;
    double scrub_fraction_ = 0.0;
};
