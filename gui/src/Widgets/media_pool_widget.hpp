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

enum MediaPoolRoles {
    kPoolMediaIndexRole = Qt::UserRole,
    kPoolIsVideoRole    = Qt::UserRole + 1,
    kPoolResolutionRole = Qt::UserRole + 2,
    kPoolDurationRole   = Qt::UserRole + 3,
    kPoolHasAudioRole   = Qt::UserRole + 4,
    kPoolWaveformImageRole = Qt::UserRole + 5,
};

class MediaPoolWidget : public QListWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(QWidget* parent = nullptr);

    void update_empty_state();

    [[nodiscard]] int scrub_index() const { return scrub_index_; }
    [[nodiscard]] double scrub_fraction() const { return scrub_fraction_; }

signals:
    void importRequested();
    void filesDropped(const QStringList& paths);
    void deleteSelectedRequested();
    void deleteSelectedWithClipsRequested();
    void clipScrubbed(int media_index, double fraction);
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
    int scrub_index_ = -1;
    double scrub_fraction_ = 0.0;
};
