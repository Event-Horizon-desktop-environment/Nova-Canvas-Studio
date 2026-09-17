#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace canvas::gui {

QString default_projects_root();

QString default_media_root();

bool ensure_project_roots();

QString sanitize_project_name(const QString& raw);

class NewProjectDialog final : public QDialog {
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget* parent = nullptr);

    [[nodiscard]] QString project_name() const;
    [[nodiscard]] QString media_location() const;

private:
    void pick_media_location();

    QLineEdit* name_edit_ = nullptr;
    QLineEdit* location_field_ = nullptr;
};

}
