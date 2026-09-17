#pragma once

namespace canvas::gui::timeline_volume_line {

constexpr double kVolumeLinePad = 0.0;

constexpr float kVolumeLineHitBandPx = 4.0f;

constexpr double kVolumeLineDragExponent = 1.5;

constexpr double kVolumeWaveformScaleFloor = 0.35;

double volume_line_y(double db, double body_h);

double db_from_volume_line_y(double y, double body_h);

double drag_db_from_relative_y(double rel_y, double body_h, double curve);

double volume_waveform_scale(double db);

}
