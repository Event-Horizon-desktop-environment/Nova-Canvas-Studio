#pragma once

#include <QString>
#include <cstdint>
#include <cmath>

namespace canvas::gui {

[[nodiscard]] inline QString timecode(const int64_t frame, const double fps) {
    if (fps <= 0.0) return QStringLiteral("--:--:--:--");
    const int fps_i = std::max(1, static_cast<int>(std::lround(fps)));
    int64_t total = frame < 0 ? 0 : frame;
    const int ff = static_cast<int>(total % fps_i);
    total /= fps_i;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(total / 3600, 2, 10, QLatin1Char('0'))
        .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

}
