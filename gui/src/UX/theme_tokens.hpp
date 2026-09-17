#pragma once

#include <QColor>
#include <QString>

#include <cstdint>
#include <cstddef>

namespace canvas::gui {

struct ThemeTokens {
    QColor surface;
    QColor surface_low;
    QColor surface_raised;
    QColor surface_higher;
    QColor surface_highest;
    QColor border;
    QColor border_soft;
    QColor border_hi;
    QColor ink;
    QColor ink_muted;
    QColor ink_faint;
    QColor icon;
    QColor accent;
    QColor accent_hover;
    QColor accent_press;
    QColor on_accent;
    QColor accent_text;
    QColor accent_soft;
    QColor accent_line;
    QColor playhead;
    QColor playhead_soft;
    QColor clip_video;
    QColor clip_audio;
    QColor clip_label;
    QColor clip_border_video;
    QColor clip_border_audio;
    QColor clip_shadow;
    QColor danger;
    QColor danger_soft;
    QColor warn;
    QColor state_hover;
    QColor state_press;
    QColor state_selected;
    QColor focus_ring;
    QString font_ui;
    QString font_mono;
};

namespace layout {
inline constexpr int kSpace4 = 4;
inline constexpr int kSpace8 = 8;
inline constexpr int kSpace12 = 12;
inline constexpr int kSpace16 = 16;
inline constexpr int kSpace24 = 24;

inline constexpr int kTypeCaption = 12;
inline constexpr int kTypeBody = 14;
inline constexpr int kTypeTitle = 16;
inline constexpr int kTypeDisplay = 20;
}

const ThemeTokens& tokens();

void log_theme_tokens();

QString css(const QColor& c);

QColor with_alpha(const QColor& c, int alpha);

enum class ThemeTokenField : uint8_t {
    Surface, SurfaceLow, SurfaceRaised, SurfaceHigher, SurfaceHighest,
    Border, BorderSoft, Ink, InkMuted, InkFaint, Icon,
    Accent, Playhead,
    ClipVideo, ClipAudio, ClipLabel, ClipBorderVideo, ClipBorderAudio,
    Danger, Warn,
    COUNT_
};
inline constexpr int kThemeTokenFieldCount =
    static_cast<int>(ThemeTokenField::COUNT_);

const char* theme_token_field_name(ThemeTokenField field);

QString theme_setting_key(ThemeTokenField field);

QString theme_color_string(const QColor& c);

void set_token_override(ThemeTokenField field, const QColor& color);
QColor token_override(ThemeTokenField field);

QColor designed_token_value(ThemeTokenField field);

bool export_theme_file(const QString& path, const QString& name);

bool import_theme_file(const QString& path, QString* name_out);

}