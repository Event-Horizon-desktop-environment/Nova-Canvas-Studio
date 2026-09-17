#pragma once

#include <QString>

#include <functional>

class QApplication;
class QWidget;

namespace canvas::gui {

bool is_light();

bool is_hypr_dark();

void set_light(bool light);

void set_hypr_dark(bool enabled);

void refresh_theme();

void register_theme_reapply(std::function<void()> fn);

void apply_theme_style(QWidget* w, const std::function<QString()>& style);

void apply_panel_shadow(QWidget* w);

void apply_theme(QApplication& app, bool light = false, bool hypr_dark = false);

}
