// Qt-free volume-line math shared by the timeline's audio-clip gain line and
// its drag hit-testing (see timeline_volume_line_test.cpp for the law's shape).
// The line maps the clip's volume_db to a vertical position inside the audio
// clip's waveform box with 0 dB dead-center: drag UP from there => louder up to
// +24 dB at the very top edge of the box, drag DOWN => quieter down to -100 dB
// (silence, db_to_gain -> 0) at the very bottom edge. Mapping the full audio law
// band across the box (no invisible visual floor) means the bottom of the
// waveform IS digital silence, and the direction is unambiguous around the
// 0 dB center. The band limits ride the box edges exactly — the "all the way to
// the bottom/top of the clip" extremes.
#pragma once

namespace canvas::gui::timeline_volume_line {

// Px of breathing room the line keeps from the box's top/bottom edges at the
// band limits. 0.0 = the +24 dB line sits dead-on the top edge and the -100 dB
// line dead-on the bottom edge (the "-100 is the clip's very bottom" behaviour).
constexpr double kVolumeLinePad = 0.0;

// Half-width of the vertical drag hit band around the line, in scene px.
constexpr float kVolumeLineHitBandPx = 4.0f;

// Drag-session curve exponent: the fractional distance from the 0 dB center is
// raised to this power before reading the dB, so the SAME gesture is fine-
// grained/slower near the center (less jumpy) while a full box-height gesture
// still reaches the band limits exactly at the box edges. A plain division
// (gear ratio) could not do both: it pushed the extremes beyond the box.
constexpr double kVolumeLineDragExponent = 1.5;

// Floor on the waveform's vertical scale: the spectrum shrinks toward this as
// the clip quiets but never collapses into a flat line (the old linear gain
// multiplied bars by db_to_gain -> 0, flattening them).
constexpr double kVolumeWaveformScaleFloor = 0.35;

// Vertical position (0..body_h, from the top of the waveform box) for a volume
// value in dB. Input is clamped to the law band; the result stays within
// [kVolumeLinePad, body_h - kVolumeLinePad].
double volume_line_y(double db, double body_h);

// Inverse of volume_line_y: the dB value for a vertical position within the box
// (y=0 top, y=body_h bottom). The position is clamped into the visible band and
// the returned dB into [audio_mix::kMinVolumeDb, kMaxVolumeDb].
double db_from_volume_line_y(double y, double body_h);

// Drag-session converter: same idea as db_from_volume_line_y but with the
// fractional distance from the center raised to `curve` (>=1) before the dB is
// read — fine-grained near 0 dB, full band reach (exactly +24 / -100) at the box
// edges. Positions beyond the box keep mapping to the law band limits.
double drag_db_from_relative_y(double rel_y, double body_h, double curve);

// Vertical scale for the clip's waveform pixmap so the spectrum tracks the
// volume: louder = fuller bars, quieter = shorter bars, monotonically across
// the whole band (-100 dB .. 0 dB), floored so it can never flatten into a line.
// 0 dB (and louder) = 1.0 full height, -100 dB = kVolumeWaveformScaleFloor.
double volume_waveform_scale(double db);

}  // namespace canvas::gui::timeline_volume_line