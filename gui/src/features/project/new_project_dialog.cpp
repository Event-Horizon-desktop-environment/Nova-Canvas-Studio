#include "features/project/new_project_dialog.hpp"

#include "UX/theme.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace canvas::gui {

QString default_projects_root() {
    const QString override = QSettings().value(QStringLiteral("projectRootDir")).toString();
    if (!override.isEmpty()) return override;
    QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (base.isEmpty()) base = QDir::homePath();
    return base + QStringLiteral("/Nova Canvas Studio");
}

QString default_media_root() {
    const QString saved = QSettings().value(QStringLiteral("mediaRootDir")).toString();
    if (!saved.isEmpty()) return saved;
    return QDir::home().filePath(QStringLiteral("Nova Canvas Studio"));
}

bool ensure_project_roots() {
    bool ok = QDir().mkpath(default_projects_root());
    ok = QDir().mkpath(default_media_root()) && ok;
    return ok;
}

QString sanitize_project_name(const QString& raw) {
    QString out = raw.trimmed();
    out.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")), QStringLiteral("-"));
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    if (out.isEmpty() || out == QStringLiteral(".") || out == QStringLiteral(".."))
        out = QStringLiteral("Untitled");
    return out;
}

NewProjectDialog::NewProjectDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("newProjectDialog"));
    setWindowTitle(tr("New Project"));
    setModal(true);
    setMinimumWidth(470);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(12);

    auto* title = new QLabel(tr("New Project"), this);
    QFont title_f = title->font();
    title_f.setPixelSize(16);
    title_f.setWeight(QFont::Bold);
    title->setFont(title_f);
    title->setStyleSheet(QStringLiteral("color: %1;").arg(css(tokens().ink)));
    root->addWidget(title);
    root->addSpacing(2);

    auto* name_label = new QLabel(tr("Project name"), this);
    name_label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 600;")
                                  .arg(css(tokens().ink_muted)));
    root->addWidget(name_label);

    name_edit_ = new QLineEdit(this);
    name_edit_->setObjectName(QStringLiteral("npName"));
    name_edit_->setPlaceholderText(tr("My Project"));
    root->addWidget(name_edit_);

    auto* loc_label = new QLabel(tr("Media location"), this);
    loc_label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 600;")
                                 .arg(css(tokens().ink_muted)));
    root->addWidget(loc_label);

    auto* loc_row = new QHBoxLayout();
    loc_row->setSpacing(8);
    location_field_ = new QLineEdit(this);
    location_field_->setObjectName(QStringLiteral("npLocation"));
    location_field_->setReadOnly(true);
    location_field_->setText(default_media_root());
    loc_row->addWidget(location_field_, 1);
    auto* change_btn = new QPushButton(tr("Change Location…"), this);
    change_btn->setObjectName(QStringLiteral("npChange"));
    loc_row->addWidget(change_btn);
    root->addLayout(loc_row);

    auto* hint = new QLabel(
        tr("Projects live in %1 — each in its own folder, saved there automatically.")
            .arg(default_projects_root()),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(css(tokens().ink_faint)));
    root->addWidget(hint);

    root->addSpacing(10);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("npCancel"));
    auto* create = new QPushButton(tr("Create Project"), this);
    create->setObjectName(QStringLiteral("npCreate"));
    create->setDefault(true);
    create->setEnabled(false);
    buttons->addWidget(cancel);
    buttons->addSpacing(12);
    buttons->addWidget(create);
    root->addLayout(buttons);

    connect(name_edit_, &QLineEdit::textChanged, this,
            [create](const QString& text) { create->setEnabled(!text.trimmed().isEmpty()); });
    connect(change_btn, &QPushButton::clicked, this, &NewProjectDialog::pick_media_location);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(create, &QPushButton::clicked, this, &QDialog::accept);

    apply_theme_style(this, [this] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
            "QWidget#newProjectDialog { background: %1; }"
            "QLineEdit#npName, QLineEdit#npLocation { background: %2; border: 1px solid %3; "
            "border-radius: 8px; padding: 7px 10px; color: %4; }"
            "QLineEdit#npName:focus, QLineEdit#npLocation:focus { border: 1px solid %5; }"
            "QPushButton#npCreate { background: %6; color: %7; font-weight: 600; "
            "border-radius: 8px; padding: 8px 16px; font-size: 12px; }"
            "QPushButton#npCreate:hover { background: %8; }"
            "QPushButton#npCreate:disabled { background: %2; color: %9; }"
            "QPushButton#npCancel, QPushButton#npChange { background: transparent; "
            "border: 1px solid %10; color: %11; border-radius: 8px; padding: 8px 16px; font-size: 12px; }"
            "QPushButton#npCancel:hover, QPushButton#npChange:hover { background: %12; }")
            .arg(css(t.surface), css(t.surface_raised), css(t.border), css(t.ink),
                 css(t.accent_line), css(t.accent), css(t.on_accent), css(t.accent_hover),
                 css(t.ink_faint), css(t.border_soft), css(t.ink_muted), css(t.surface_higher));
    });
}

void NewProjectDialog::pick_media_location() {
    const QString current = location_field_->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose Media Location"), current.isEmpty() ? default_media_root() : current,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) location_field_->setText(dir);
}

QString NewProjectDialog::project_name() const {
    return name_edit_->text().trimmed();
}

QString NewProjectDialog::media_location() const {
    const QString loc = location_field_->text().trimmed();
    return loc.isEmpty() ? default_media_root() : loc;
}

}