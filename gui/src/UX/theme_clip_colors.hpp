#pragma once

// Resolve-style clip colour swatches shared by the File-inspector picker and
// the timeline clip stripe so the two never drift.

#include <QColor>

#include <cstdint>

namespace canvas::gui {

// 12 entries, indexed 0-11 = colour 1-12.
const QColor* clip_color_swatches();

// 0 -> invalid QColor (no colour).
QColor clip_color_for(uint8_t color);

}  // namespace canvas::gui