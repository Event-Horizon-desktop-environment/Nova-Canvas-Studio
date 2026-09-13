#pragma once

// Settings / Preferences dialog. App-level settings that don't belong in a
// menu stack: playback behavior (audible scrubbing), the hardware-decode
// backend preference, and live theme-token overrides (accent + playhead).
//
// Every change applies immediately AND persists via QSettings (default scope:
// org "Nova Canvas", app "canvas" — same bucket as appearance/theme and
// scrubAudioEnabled). There is deliberately no OK/Cancel: like the Appearance
// menu radio group, the settings are live, and Close just dismisses. This
// mirrors how the app already treats the theme switches as instant.
//
// Keys written:
//   scrubAudioEnabled      bool   (also used by the old popup; kept)
//   settings/hw_backend    string "" (auto) | "cuda"|"vaapi"|"qsv"|"vulkan"|"software"
//   settings/accent_color  string hex (#rrggbb) or empty
//   settings/playhead_color string hex (#rrggbb) or empty
//
// The hardware-decode preference is handed to the core HwDeviceManager as a
// process-wide pin (see set_preferred_backend), so startup probes and every
// manager created afterwards honor it. A manager that already opened a device
// keeps it until it closes (project switch); the note in the UI says so.

#include <QDialog>

#include <functional>

class QCheckBox;
class QColor;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QPushButton;

namespace canvas::gui {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    // `audible_scrubbing_cb` wires the Audible Scribbing toggle into the
    // controller (set_scrub_audio_enabled); nullptr skips the field.
    explicit SettingsDialog(QWidget* parent,
                            std::function<void(bool)> audible_scrubbing_cb);

private:
    QGroupBox* build_playback_section();
    QGroupBox* build_hardware_section();
    QGroupBox* build_appearance_section();

    // Color row: a swatch button + per-row Reset. `get_current` returns the live
    // token value, `design_token` the mode's designed color (shown/seed when no
    // override is set), `apply` stores + refreshes.
    void add_color_row(QFormLayout* form, const QString& title,
                       const std::function<QColor()>& get_current,
                       const std::function<QColor()>& design_token,
                       const std::function<void(const QColor&)>& apply,
                       const QString& settings_key);

    QPushButton* make_swatch();

    std::function<void(bool)> audible_scrubbing_cb_;
    QCheckBox* scrub_audio_ = nullptr;
    QComboBox* backend_combo_ = nullptr;
};

}  // namespace canvas::gui