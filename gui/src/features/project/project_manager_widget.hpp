#pragma once

// DaVinci Resolve-style Project Manager, presented as its own floating window
// (the app's first screen). A grid of the user's recent projects (.ncs files
// from QSettings "recentProjects") plus a leading "+ New Project" tile, a
// search + sort row, and the footer actions (New / Import). A project opens on
// double-click (or Enter). Card thumbnails are decoded off-thread on the
// shared ThumbnailService: the widget emits thumbnail_requested(token, path),
// MainWindow loads the .ncs, asks for a frame from its first video media, and
// set_card_thumbnail(token, image) lands the finished frame back on its tile.

#include <QDialog>
#include <QWidget>

#include <cstdint>
#include <vector>

class QCloseEvent;
class QImage;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QComboBox;

namespace canvas::gui {

// Namespace bit for ThumbnailService preview ids owned by Project Manager
// cards. The pool and timeline share one id space, so the manager's cards OR
// this high-bit prefix in — and the pool's ~kPoolThumbNs mask leaves the card
// id far out of any pool index range, so a project-card frame can never land
// on a pool cell or timeline filmstrip.
inline constexpr std::uint64_t kProjectThumbNs = 0xE000000000000000ULL;

// Roles on the grid items (UserRole base keeps clear of the pool's roles).
inline constexpr int kProjectPathRole = Qt::UserRole + 1;    // .ncs absolute path
inline constexpr int kProjectMetaRole = Qt::UserRole + 2;    // QDateTime of last modified
inline constexpr int kProjectMissingRole = Qt::UserRole + 3; // file no longer exists
inline constexpr int kProjectNewRole = Qt::UserRole + 4;     // "+ New Project" tile
inline constexpr int kProjectTokenRole = Qt::UserRole + 5;   // 1-based thumbnail token
inline constexpr int kProjectThumbRole = Qt::UserRole + 6;   // QImage card thumbnail

// The Project Manager page. Pure view + grid logic; project loading and
// thumbnail decode stay in MainWindow (the thumbnail bridge is signal-driven so
// this widget never touches core).
class ProjectManagerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectManagerWidget(QWidget* parent = nullptr);

    // Rebuilds the card grid from QSettings "recentProjects". The "+ New
    // Project" tile always leads, then one card per recent project (newest
    // first, or sorted per the toolbar combo). Safe to call while visible
    // (Home-button re-entry).
    void refresh();

    // Lands a finished ThumbnailService frame on the card that requested it.
    void set_card_thumbnail(int token, const QImage& image);

signals:
    void open_project_requested(const QString& path);
    void new_project_requested();
    void import_project_requested();
    // Asks MainWindow to load the .ncs at `project_path` and request a frame
    // from its first video media under `token` (route id kProjectThumbNs | token).
    void thumbnail_requested(int token, const QString& project_path);

private:
    void add_new_project_tile();
    void add_project_card(const QString& path, int token);
    void filter_grid(const QString& text);
    void apply_sort();
    void activate_item(QListWidgetItem* item);
    void activate_current();
    void open_context_menu(const QPoint& global_pos);

    QLabel* empty_hint_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* sort_combo_ = nullptr;
    QListWidget* grid_ = nullptr;
    std::vector<QListWidgetItem*> cards_;
};

// The Project Manager window — the app's own top-level gateway screen,
// parented to MainWindow so it dies with the app, NOT a page inside the
// editor. A "Local" tab strip + search/sort row sit in a header above the
// project grid. MainWindow lazily creates one, shows it on enter_project_manager
// and hides it on leave_project_manager; closing it (X or Esc) routes back to
// the editor via window_closed.
class ProjectManagerWindow final : public QDialog {
    Q_OBJECT

public:
    explicit ProjectManagerWindow(QWidget* parent = nullptr);

    [[nodiscard]] ProjectManagerWidget* content() const { return widget_; }

    // Rebuilds the card grid (Home-button re-entry).
    void refresh() {
        if (widget_) widget_->refresh();
    }

signals:
    // Emitted when the window's close (X) is requested. MainWindow treats it
    // as leave_project_manager(): hide the manager, return to the editor.
    void window_closed();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    ProjectManagerWidget* widget_ = nullptr;
};

}  // namespace canvas::gui