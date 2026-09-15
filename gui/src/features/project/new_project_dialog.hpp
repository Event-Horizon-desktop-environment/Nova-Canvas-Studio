#pragma once

// New-project flow. Clicking "+ New Project" (or File > New Project) pops a
// small dialog asking for two things: the project name, and the media location
// the project's footage will live in (a "Change Location…" button browses).
// Every project gets its own folder under the projects root, and the chosen
// media location is remembered both in the core Project (Project::media_root)
// and in QSettings so the next new project proposes it again.

#include <QDialog>
#include <QString>

class QLineEdit;

namespace canvas::gui {

// Root that keeps every Nova project: <user Videos>/Nova Canvas Studio. The
// "projectRootDir" QSettings key overrides it if ever set.
QString default_projects_root();

// Default media location for new projects: <home>/Nova Canvas Studio. The
// "mediaRootDir" QSettings key overrides it once the user picks one.
QString default_media_root();

// Makes both roots exist (created on demand). Returns false if creation failed.
bool ensure_project_roots();

// Sanitizes a user-typed name into a filesystem-safe folder/file name
// (path separators and other hostile characters replaced; empty -> "Untitled").
QString sanitize_project_name(const QString& raw);

// The "New Project" setup dialog: name + media location rows. Accept returns
// the result via project_name() / media_location().
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

}  // namespace canvas::gui