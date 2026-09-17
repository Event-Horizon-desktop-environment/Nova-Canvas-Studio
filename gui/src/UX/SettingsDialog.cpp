#include "UX/SettingsDialog.hpp"

#include "UX/theme.hpp"
#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

#include <array>
#include <utility>
#include <vector>

namespace canvas::gui {

namespace {

struct TokenSection {
    const char* title;
    std::vector<TokenEntry> entries;
};

const std::array<TokenSection, 5>& kTokenSections() {
    static const std::array<TokenSection, 5> sections = {{
        {"Surfaces", {
            {"Base surface", ThemeTokenField::Surface},
            {"Recessed wells", ThemeTokenField::SurfaceLow},
            {"Panels and buttons", ThemeTokenField::SurfaceRaised},
            {"Hovered surfaces", ThemeTokenField::SurfaceHigher},
            {"Menus and popups", ThemeTokenField::SurfaceHighest},
        }},
        {"Text and Borders", {
            {"Primary text", ThemeTokenField::Ink},
            {"Secondary text", ThemeTokenField::InkMuted},
            {"Tertiary text", ThemeTokenField::InkFaint},
            {"Icons (SVG glyphs)", ThemeTokenField::Icon},
            {"Borders", ThemeTokenField::Border},
            {"Hairlines", ThemeTokenField::BorderSoft},
        }},
        {"Accent and Playhead", {
            {"Accent", ThemeTokenField::Accent},
            {"Playhead", ThemeTokenField::Playhead},
        }},
        {"Timeline Clips", {
            {"Video clip", ThemeTokenField::ClipVideo},
            {"Audio clip", ThemeTokenField::ClipAudio},
            {"Clip label", ThemeTokenField::ClipLabel},
            {"Video clip border", ThemeTokenField::ClipBorderVideo},
            {"Audio clip border", ThemeTokenField::ClipBorderAudio},
        }},
        {"Status", {
            {"Danger", ThemeTokenField::Danger},
            {"Warning", ThemeTokenField::Warn},
        }},
    }};
    return sections;
}

}

SettingsDialog::SettingsDialog(QWidget* parent,
                               std::function<void(bool)> audible_scrubbing_cb)
    : QDialog(parent),
      audible_scrubbing_cb_(std::move(audible_scrubbing_cb)) {
    setWindowTitle(tr("Settings"));
    setModal(true);
    resize(560, 720);
    setMinimumSize(520, 600);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(12);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(build_general_tab(), tr("General"));
    tabs->addTab(build_theme_tab(), tr("Theme"));
    root->addWidget(tabs);

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
    auto* box = new QGroupBox(tr("Hardware Acceleration"), this);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(12, 16, 12, 12);

    gpu_combo_ = new QComboBox(box);
    gpu_combo_->addItem(tr("Automatic (best available)"), QStringLiteral(""));
    const std::string cpu_name =
        canvas::core::gpu_select::cpu_name();
    QString cpu_label = cpu_name.empty()
                            ? tr("CPU (encode + decode)")
                            : QString::fromUtf8(cpu_name.c_str()) +
                                  tr(" \u00b7 CPU (encode + decode)");
    gpu_combo_->addItem(
        cpu_label,
        QString::fromUtf8(canvas::core::gpu_select::kCpuSentinel));
    const auto gpus = canvas::core::gpu_select::detect_gpus();
    for (const auto& g : gpus) {
        QString label = QString::fromUtf8(g.name.c_str());
        if (!g.backend.empty())
            label += QStringLiteral(" \u00b7 ") +
                     QString::fromUtf8(g.backend.c_str());
        gpu_combo_->addItem(label, QString::fromUtf8(g.pci_slot.c_str()));
    }
    const QSettings settings;
    const QString gpu_now = settings
        .value(QStringLiteral("settings/hw_gpu"), QStringLiteral(""))
        .toString();
    const int gpu_idx = gpu_combo_->findData(gpu_now);
    gpu_combo_->setCurrentIndex(gpu_idx >= 0 ? gpu_idx : 0);
    const bool cpu_pinned =
        gpu_now ==
        QString::fromUtf8(canvas::core::gpu_select::kCpuSentinel);
    const bool gpu_pinned =
        gpu_idx > 0 && !cpu_pinned;
    const canvas::core::gpu_select::GpuDevice* pinned = nullptr;
    if (gpu_pinned)
        for (const auto& g : gpus)
            if (g.pci_slot == gpu_now.toStdString()) pinned = &g;

    backend_combo_ = new QComboBox(box);
    backend_combo_->addItem(tr("Automatic (probe order)"), QStringLiteral(""));

    bool have_amd = false, have_intel = false, have_nvidia = false, any_gpu = false;
    for (const auto& g : gpus) {
        any_gpu = true;
        if (g.vendor == "AMD") have_amd = true;
        else if (g.vendor == "Intel") have_intel = true;
        else if (g.vendor == "NVIDIA") have_nvidia = true;
    }
    if (have_nvidia)
        backend_combo_->addItem(tr("NVIDIA CUDA"), QStringLiteral("cuda"));
    if (have_amd || have_intel) {
        QString vaapi_label = have_amd ? QStringLiteral("AMD") : QString();
        if (have_intel)
            vaapi_label += vaapi_label.isEmpty() ? QStringLiteral("Intel")
                                                 : QStringLiteral(" / Intel");
        backend_combo_->addItem(vaapi_label + tr(" VAAPI"),
                                QStringLiteral("vaapi"));
    }
    if (have_intel)
        backend_combo_->addItem(tr("Intel QSV"), QStringLiteral("qsv"));
    if (any_gpu)
        backend_combo_->addItem(tr("Vulkan Video"), QStringLiteral("vulkan"));
    backend_combo_->addItem(tr("Software (no GPU decode)"),
                            QStringLiteral("software"));

    QString backend_now = settings
        .value(QStringLiteral("settings/hw_backend"), QStringLiteral(""))
        .toString();
    if (pinned)
        backend_now = QString::fromStdString(pinned->backend);
    if (cpu_pinned) backend_now = QStringLiteral("software");
    const int idx = backend_combo_->findData(backend_now);
    backend_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
    backend_combo_->setEnabled(!gpu_pinned && !cpu_pinned);

    auto* hint = new QLabel(
        tr("Preferred hardware accelerator. \"GPU\" pins one physical device; "
           "its backend is chosen for you. The CPU row pins pure software "
           "encode + decode on the named processor. \"Decoder\" pins a "
           "backend family when the GPU is Automatic. Applies on the next "
           "decode session (a device already open keeps working until it "
           "closes)."),
        box);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    form->addRow(tr("GPU"), gpu_combo_);
    form->addRow(tr("Decoder"), backend_combo_);
    form->addRow(QString(), hint);

    connect(gpu_combo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString slot = gpu_combo_->itemData(i).toString();
        QSettings().setValue(QStringLiteral("settings/hw_gpu"), slot);
        if (slot.isEmpty()) {
            backend_combo_->setEnabled(true);
            const QString backend = backend_combo_->currentData().toString();
            canvas::core::HwDeviceManager::set_preferred_backend(
                backend.toStdString());
            return;
        }
        if (slot == QString::fromUtf8(
                          canvas::core::gpu_select::kCpuSentinel)) {
            backend_combo_->setEnabled(false);
            const int b_idx =
                backend_combo_->findData(QStringLiteral("software"));
            if (b_idx >= 0) backend_combo_->setCurrentIndex(b_idx);
            canvas::core::HwDeviceManager::set_preferred_backend("software");
            return;
        }
        backend_combo_->setEnabled(false);
        const auto gpus = canvas::core::gpu_select::detect_gpus();
        for (const auto& g : gpus) {
            if (g.pci_slot == slot.toStdString()) {
                const int b_idx = backend_combo_->findData(
                    QString::fromStdString(g.backend));
                if (b_idx >= 0) backend_combo_->setCurrentIndex(b_idx);
                canvas::core::HwDeviceManager::set_preferred_gpu(
                    g.backend, g.device_arg);
                return;
            }
        }
        backend_combo_->setEnabled(true);
        const QString backend = backend_combo_->currentData().toString();
        canvas::core::HwDeviceManager::set_preferred_backend(
            backend.toStdString());
    });

    connect(backend_combo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString value = backend_combo_->itemData(i).toString();
        QSettings().setValue(QStringLiteral("settings/hw_backend"), value);
        canvas::core::HwDeviceManager::set_preferred_backend(
            value.toStdString());
    });

    return box;
}

QWidget* SettingsDialog::build_general_tab() {
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);
    layout->addWidget(build_playback_section());
    layout->addWidget(build_hardware_section());
    layout->addStretch(1);
    return tab;
}

QGroupBox* SettingsDialog::build_theme_mode_group() {
    auto* box = new QGroupBox(tr("Appearance Mode"), this);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(12, 14, 12, 12);

    auto* group = new QButtonGroup(box);
    group->setExclusive(true);
    auto* dark = new QRadioButton(tr("Dark"), box);
    auto* hypr = new QRadioButton(tr("Dark \u00b7Hyprland"), box);
    auto* light = new QRadioButton(tr("Light"), box);

    const bool light_now = is_light();
    const bool hypr_now = is_hypr_dark();
    dark->setChecked(!light_now && !hypr_now);
    hypr->setChecked(!light_now && hypr_now);
    light->setChecked(light_now);

    group->addButton(dark);
    group->addButton(hypr);
    group->addButton(light);
    row->addWidget(dark);
    row->addWidget(hypr);
    row->addWidget(light);
    row->addStretch(1);

    const auto wire = [this](QRadioButton* btn, bool light, bool hypr) {
        connect(btn, &QRadioButton::toggled, this,
                [this, light, hypr](bool on) {
                    if (on) apply_mode(light, hypr);
                });
    };
    wire(dark, false, false);
    wire(hypr, false, true);
    wire(light, true, false);
    return box;
}

QWidget* SettingsDialog::build_theme_tab() {
    auto* theme = new QWidget(this);
    auto* tv = new QVBoxLayout(theme);
    tv->setContentsMargins(12, 12, 12, 12);
    tv->setSpacing(12);

    tv->addWidget(build_theme_mode_group());

    auto* scroll = new QScrollArea(theme);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 4, 0);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    int row = 0;
    for (const auto& section : kTokenSections())
        add_token_section(grid, row, QLatin1String(section.title),
                          section.entries);

    grid->setRowStretch(row, 1);
    scroll->setWidget(host);
    tv->addWidget(scroll, 1);

    auto* actions = new QHBoxLayout;
    auto* reset_all = new QPushButton(tr("Reset Theme"), theme);
    auto* import_btn = new QPushButton(tr("Import Theme..."), theme);
    auto* save_btn = new QPushButton(tr("Save Theme..."), theme);
    actions->addWidget(reset_all);
    actions->addWidget(import_btn);
    actions->addWidget(save_btn);
    actions->addStretch(1);
    tv->addLayout(actions);

    connect(reset_all, &QPushButton::clicked, this, [this] {
        QSettings settings;
        for (int i = 0; i < kThemeTokenFieldCount; ++i) {
            const auto f = static_cast<ThemeTokenField>(i);
            set_token_override(f, QColor());
            settings.setValue(theme_setting_key(f), QString());
        }
        refresh_theme();
        resync_token_rows();
    });

    connect(import_btn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Theme"), QString(),
            tr("Nova Canvas Theme (*.json *.canvas-theme);;All Files (*)"));
        if (path.isEmpty()) return;
        QString name;
        if (!import_theme_file(path, &name)) {
            QMessageBox::warning(
                this, tr("Import Theme"),
                tr("Not a valid Nova Canvas theme file."));
            return;
        }
        QSettings settings;
        for (int i = 0; i < kThemeTokenFieldCount; ++i) {
            const auto f = static_cast<ThemeTokenField>(i);
            const QColor c = token_override(f);
            settings.setValue(theme_setting_key(f),
                              c.isValid() ? theme_color_string(c) : QString());
        }
        refresh_theme();
        resync_token_rows();
    });

    connect(save_btn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Save Theme"), tr("Theme name:"), QLineEdit::Normal,
            QStringLiteral("Nova Theme"), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Theme"),
            name.trimmed() + QStringLiteral(".canvas-theme.json"),
            tr("Nova Canvas Theme (*.json *.canvas-theme);;All Files (*)"));
        if (path.isEmpty()) return;
        if (!export_theme_file(path, name.trimmed()))
            QMessageBox::warning(this, tr("Save Theme"),
                                 tr("Could not write the theme file."));
    });

    return theme;
}

void SettingsDialog::add_token_section(QGridLayout* grid, int& row,
                                       const QString& title,
                                       const std::vector<TokenEntry>& entries) {
    if (row > 0) {
        grid->setRowMinimumHeight(row, 10);
        ++row;
    }
    auto* lbl = new QLabel(title);
    apply_theme_style(lbl, [] {
        return QStringLiteral(
            "QLabel { color: %1; font-size: 11px; font-weight: 600;"
            " letter-spacing: 0.08em; background: transparent; }")
            .arg(css(tokens().ink_muted));
    });
    grid->addWidget(lbl, row, 0, 1, 2);
    ++row;

    for (const auto& e : entries) {
        grid->addWidget(new QLabel(QLatin1String(e.label)), row, 0);
        grid->addWidget(build_color_row(e.field), row, 1);
        ++row;
    }
}

QWidget* SettingsDialog::build_color_row(ThemeTokenField field) {
    auto* row = new QWidget(this);

    auto* swatch = make_swatch();
    const QColor designed = designed_token_value(field);
    auto swatch_qss = [](const QColor& c) {
        return QStringLiteral(
                   "background-color: %1; border: 1px solid %2;"
                   " border-radius: 4px;")
            .arg(css(c), css(c.darker(140)));
    };
    swatch->setStyleSheet(swatch_qss(designed));
    auto update_bg = [swatch, designed, swatch_qss](const QColor& c) {
        const QColor shown = c.isValid() ? c : designed;
        swatch->setStyleSheet(swatch_qss(shown));
    };

    auto* reset = new QPushButton(tr("Reset"), this);
    reset->setEnabled(token_override(field).isValid());

    const auto apply_override = [this, field](const QColor& raw) {
        QColor stored = raw;
        if (stored.isValid()) {
            const QColor d = designed_token_value(field);
            stored = QColor(stored.red(), stored.green(), stored.blue(),
                            d.alpha());
        }
        set_token_override(field, stored);
        refresh_theme();
        QSettings().setValue(theme_setting_key(field),
                             stored.isValid() ? theme_color_string(stored)
                                              : QString());
    };

    connect(swatch, &QPushButton::clicked, this,
            [this, update_bg, reset, field, designed, apply_override] {
                const QColor picked = QColorDialog::getColor(
                    designed, this, QStringLiteral("Pick a color"));
                if (!picked.isValid()) return;
                apply_override(picked);
                update_bg(picked);
                reset->setEnabled(true);
            });
    connect(reset, &QPushButton::clicked, this,
            [update_bg, reset, apply_override] {
                apply_override(QColor());
                update_bg(QColor());
                reset->setEnabled(false);
            });

    theme_resyncs_.push_back([update_bg, reset, field] {
        update_bg(QColor());
        reset->setEnabled(token_override(field).isValid());
    });

    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(8);
    hl->addStretch(1);
    hl->addWidget(swatch);
    hl->addWidget(reset);
    return row;
}

QPushButton* SettingsDialog::make_swatch() {
    auto* swatch = new QPushButton(this);
    swatch->setFixedSize(56, 26);
    swatch->setCursor(Qt::PointingHandCursor);
    return swatch;
}

void SettingsDialog::apply_mode(bool light, bool hypr) {
    set_light(light);
    set_hypr_dark(hypr);
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/theme"),
                      light ? QStringLiteral("light") : QStringLiteral("dark"));
    settings.setValue(QStringLiteral("appearance/hypr_dark"), hypr);
    resync_token_rows();
}

void SettingsDialog::resync_token_rows() {
    for (const auto& fn : theme_resyncs_) fn();
}

}