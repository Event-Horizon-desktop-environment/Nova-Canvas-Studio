#pragma once

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

inline constexpr std::uint64_t kProjectThumbNs = 0xE000000000000000ULL;

inline constexpr int kProjectPathRole = Qt::UserRole + 1;
inline constexpr int kProjectMetaRole = Qt::UserRole + 2;
inline constexpr int kProjectMissingRole = Qt::UserRole + 3;
inline constexpr int kProjectNewRole = Qt::UserRole + 4;
inline constexpr int kProjectTokenRole = Qt::UserRole + 5;
inline constexpr int kProjectThumbRole = Qt::UserRole + 6;

class ProjectManagerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectManagerWidget(QWidget* parent = nullptr);

    void refresh();

    void set_card_thumbnail(int token, const QImage& image);

signals:
    void open_project_requested(const QString& path);
    void new_project_requested();
    void import_project_requested();
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

class ProjectManagerWindow final : public QDialog {
    Q_OBJECT

public:
    explicit ProjectManagerWindow(QWidget* parent = nullptr);

    [[nodiscard]] ProjectManagerWidget* content() const { return widget_; }

    void refresh() {
        if (widget_) widget_->refresh();
    }

signals:
    void window_closed();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    ProjectManagerWidget* widget_ = nullptr;
};

}
