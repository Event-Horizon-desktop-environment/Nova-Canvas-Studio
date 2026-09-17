#pragma once

#include "canvas/core/actions/action_registry.hpp"

#include <QDialog>
#include <QHash>
#include <QString>

class QLineEdit;
class QListWidget;
class QMenuBar;
class QAction;

namespace canvas::gui {

class ActionSearch final : public QDialog {
    Q_OBJECT

public:
    explicit ActionSearch(QMenuBar* menubar, QWidget* parent = nullptr);

    void collect_actions();

private:
    void apply_filter(const QString& query);
    void run_current();

    QMenuBar* menubar_ = nullptr;
    QLineEdit* query_ = nullptr;
    QListWidget* results_ = nullptr;
    canvas::core::actions::Registry registry_;
    QHash<QString, QAction*> actions_;
};

}
