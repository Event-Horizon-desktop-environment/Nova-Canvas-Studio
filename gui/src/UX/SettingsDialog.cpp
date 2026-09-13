#include "UX/SettingsDialog.hpp"

#include "UX/theme.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace canvas::gui {

SettingsDialog::SettingsDialog(QWidget* parent,
                               std::function<void(bool)> audible_scrubbing_cb)
    : QDialog(parent),
      audible_scrubbing_cb_(std::move(audible_scrubbing_cb)) {
    setWindowTitle(tr("Settings"));
    setModal(true);
    setMinimumWidth(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(12);

    root->addWidget(build_playback_section());
    root->addWidget(build_hardware_section());
    root->addWidget(build_appearance_section());
    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    root->addWidget(buttons);
}

QGroupBox* SettingsDialog::build_playback_section() {
    auto* box = new QGroupBox(tr("Playback"), this);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(12, 16, 12, 12);

    scrub_audio_ = new QCheckBox(tr("Audible Scrubbing"), box);
    const bool saved = QSettings()
        .value(QStringLiteral("scrubAudioEnabled"), true)
        .toBool();
    scrub_audio_->setChecked(saved);
    if (audible_scrubbing_cb_) audible_scrubbing_cb_(saved);
    connect(scrub_audio_, &QCheckBox::toggled, this, [this](bool on) {
        if (audible_scrubbing_cb_) audible_scrubbing_cb_(on);
        QSettings().setValue(QStringLiteral("scrubAudioEnabled"), on);
    });
    auto* hint = new QLabel(
        tr("Hear clip audio while scrubbing the timeline or viewer."), box);
    hint->setEnabled(false);
    form->addRow(QString(), scrub_audio_);
    form->addRow(QString(), hint);
    return box;
}

QGroupBox* SettingsDialog::build_hardware_section() {
    auto* box = new QGroupBox(tr("Hardware Decoding"), this);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(12, 16, 12, 12);

    backend_combo_ = new QComboBox(box);
    // Order mirrors the app's default probe order (auto) plus the software cap.
    struct Backend {
        const char* label;
        const char* value;
    };
    constexpr std::array<Backend, 6> kBackends = {{
        {"Automatic (probe order)", ""},
        {"NVIDIA CUDA", "cuda"},
        {"AMD / Intel VAAPI", "vaapi"},
        {"Intel QSV", "qsv"},
        {"Vulkan Video", "vulkan"},
        {"Software (no GPU decode)", "software"},
    }};
    for (const auto& b : kBackends)
        backend_combo_->addItem(QString::fromUtf8(b.label),
                                QString::fromUtf8(b.value));

    const QSettings settings;
    const QString current = settings
        .value(QStringLiteral("settings/hw_backend"), QStringLiteral(""))
        .toString();
    const int idx = backend_combo_->findData(current);
    backend_combo_->setCurrentIndex(idx >= 0 ? idx : 0);

    auto* hint = new QLabel(
        tr("Preferred hardware decoder. \"Automatic\" uses the built-in probe "
           "order; the rest pin that backend first. Applies on the next decode "
           "session (a device already open keeps working until it closes)."),
        box);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    form->addRow(tr("Decoder"), backend_combo_);
    form->addRow(QString(), hint);

    connect(backend_combo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString value = backend_combo_->itemData(i).toString();
        QSettings().setValue(QStringLiteral("settings/hw_backend"), value);
        canvas::core::HwDeviceManager::set_preferred_backend(
            value.toStdString());
    });

    return box;
}

QGroupBox* SettingsDialog::build_appearance_section() {
    auto* box = new QGroupBox(tr("Appearance"), this);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(12, 16, 12, 12);

    add_color_row(
        form, tr("Accent"),
        [] { return accent_override(); },
        [] { return tokens().accent; },
        [](const QColor& c) {
            set_accent_override(c);
            refresh_theme();
        },
        QStringLiteral("settings/accent_color"));

    add_color_row(
        form, tr("Playhead"),
        [] { return playhead_override(); },
        [] { return tokens().playhead; },
        [](const QColor& c) {
            set_playhead_override(c);
            refresh_theme();
        },
        QStringLiteral("settings/playhead_color"));

    auto* hint = new QLabel(
        tr("Overrides ride along in every appearance mode (Dark, Dark "
           "\u00b7Hyprland, Light). Use Reset to return a token to its "
           "designed value."),
        box);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    form->addRow(QString(), hint);
    return box;
}

QPushButton* SettingsDialog::make_swatch() {
    auto* swatch = new QPushButton(this);
    swatch->setFixedSize(56, 26);
    swatch->setCursor(Qt::PointingHandCursor);
    return swatch;
}

void SettingsDialog::add_color_row(QFormLayout* form, const QString& title,
                                   const std::function<QColor()>& get_current,
                                   const std::function<QColor()>& design_token,
                                   const std::function<void(const QColor&)>& apply,
                                   const QString& settings_key) {
    auto* row = new QHBoxLayout;

    auto* swatch = make_swatch();
    const QSettings settings;
    const QColor persisted = QColor(
        settings.value(settings_key, QStringLiteral("")).toString());
    const QColor initial = persisted.isValid()
        ? persisted
        : (get_current().isValid() ? get_current() : design_token());
    swatch->setStyleSheet(QStringLiteral("background-color: %1;")
                              .arg(css(initial)));
    auto update_bg = [swatch, design_token](const QColor& c) {
        const QColor shown = c.isValid() ? c : design_token();
        swatch->setStyleSheet(QStringLiteral("background-color: %1;")
                                  .arg(css(shown)));
    };
    row->addWidget(swatch);

    auto* reset = new QPushButton(tr("Reset"), this);
    reset->setEnabled(persisted.isValid());
    row->addWidget(reset);
    row->addStretch(1);

    connect(swatch, &QPushButton::clicked, this,
            [this, update_bg, reset, get_current, design_token, apply, settings_key] {
                const QColor start = get_current();
                const QColor use_pick = start.isValid() ? start : design_token();
                const QColor picked = QColorDialog::getColor(use_pick, this,
                                                             tr("Pick color"));
                if (!picked.isValid()) return;
                apply(picked);
                update_bg(picked);
                reset->setEnabled(true);
                QSettings().setValue(settings_key, picked.name(QColor::HexRgb));
            });
    connect(reset, &QPushButton::clicked, this,
            [this, update_bg, reset, apply, settings_key] {
                apply(QColor());
                update_bg(QColor());
                reset->setEnabled(false);
                QSettings().setValue(settings_key, QString());
            });

    auto* container = new QWidget(this);
    container->setLayout(row);
    form->addRow(title, container);
}

}  // namespace canvas::gui