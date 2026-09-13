#pragma once

// Settings / Preferences dialog. Tabbed: a "General" tab for app-level
// settings that don't belong in a menu stack (playback behavior, the
// hardware-decode backend) and a "Theme" tab for the appearance mode plus the
// full overridable token set — every color the UI can be painted with —
// including shareable theme Save/Import.
//
// Every change applies immediately AND persists via QSettings (default scope:
// org "Nova Canvas", app "canvas" — same bucket as appearance/theme and
// scrubAudioEnabled). There is deliberately no OK/Cancel: like the Appearance
// menu radio group, the settings are live, and Close just dismisses. This
// mirrors how the app already treats the theme switches as instant.
//
// Keys written:
//   scrubAudioEnabled        bool   (also used by the old popup; kept)
//   settings/hw_backend      string "" (auto) | "cuda"|"vaapi"|"qsv"|"vulkan"|"software"
//   appearance/theme         string "light" | "dark"      (theme-tab mode row)
//   appearance/hypr_dark     bool                         (theme-tab mode row)
//   settings/theme/<field>   string hex, empty = designed (see theme_tokens)
//
// The hardware-decode preference is handed to the core HwDeviceManager as a
// process-wide pin (see set_preferred_backend), so startup probes and every
// manager created afterwards honor it. A manager that already opened a device
// keeps it until it closes (project switch); the note in the UI says so.

#include <QDialog>

#include <functional>
#include <vector>

#include "UX/theme_tokens.hpp"

class QCheckBox;
class QColor;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QPushButton;
class QVBoxLayout;

namespace canvas::gui {

// One overridable token row: the human-readable label and its field. The
// Theme tab renders the fixed section/entry table (see SettingsDialog.cpp).
struct TokenEntry {
    const char* label;
    ThemeTokenField field;
};

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    // `audible_scrubbing_cb` wires the Audible Scribbling toggle into the
    // controller (set_scrub_audio_enabled); nullptr skips the field.
    explicit SettingsDialog(QWidget* parent,
                            std::function<void(bool)> audible_scrubbing_cb);

private:
    QGroupBox* build_playback_section();
    QGroupBox* build_hardware_section();
    QWidget* build_general_tab();

    // One overridable token row: the human-readable label and its field. The
    // Theme tab renders the fixed section/entry table (see SettingsDialog.cpp)
    // — keep these two in step. All sections share ONE QGridLayout so the name
    // column and the swatch column line up straight from top to bottom.
    QGroupBox* build_theme_mode_group();
    QWidget* build_theme_tab();
    void add_token_section(QGridLayout* grid, int& row, const QString& title,
                           const std::vector<TokenEntry>& entries);

    // Color row: a field-backed swatch button + per-row Reset, laid out as
    // [label column][stretch MindReset on the right], i.e. the swatch hugs the
    // right edge of the shared action column. The swatch seeds from the live
    // override (token_override(field)), falling back to the mode's designed
    // token value; choosing a color applies it (persisted under
    // settings/theme/<field>, alpha kept for translucent tokens, app refreshed
    // live). Each row registers into theme_resyncs_ so a mode flip or an
    // import re-syncs its swatch against the new token values.
    QWidget* build_color_row(ThemeTokenField field);

    QPushButton* make_swatch();

    // Flips the appearance mode the same way the menu does (persists the two
    // appearance keys), then re-syncs every token swatch against the new mode.
    void apply_mode(bool light, bool hypr);
    // Re-runs each registered row's swatch update against its live token.
    void resync_token_rows();

    std::function<void(bool)> audible_scrubbing_cb_;
    QCheckBox* scrub_audio_ = nullptr;
    QComboBox* backend_combo_ = nullptr;
    std::vector<std::function<void()>> theme_resyncs_;
};

}  // namespace canvas::gui