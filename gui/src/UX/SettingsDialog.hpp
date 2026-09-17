#pragma once

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

struct TokenEntry {
    const char* label;
    ThemeTokenField field;
};

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent,
                            std::function<void(bool)> audible_scrubbing_cb);

private:
    QGroupBox* build_playback_section();
    QGroupBox* build_hardware_section();
    QWidget* build_general_tab();

    QGroupBox* build_theme_mode_group();
    QWidget* build_theme_tab();
    void add_token_section(QGridLayout* grid, int& row, const QString& title,
                           const std::vector<TokenEntry>& entries);

    QWidget* build_color_row(ThemeTokenField field);

    QPushButton* make_swatch();

    void apply_mode(bool light, bool hypr);
    void resync_token_rows();

    std::function<void(bool)> audible_scrubbing_cb_;
    QCheckBox* scrub_audio_ = nullptr;
    QComboBox* backend_combo_ = nullptr;
    QComboBox* gpu_combo_ = nullptr;
    std::vector<std::function<void()>> theme_resyncs_;
};

}
