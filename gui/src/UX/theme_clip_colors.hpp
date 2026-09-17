#pragma once

#include <QColor>

#include <cstdint>

namespace canvas::gui {

const QColor* clip_color_swatches();

QColor clip_color_for(uint8_t color);

}
