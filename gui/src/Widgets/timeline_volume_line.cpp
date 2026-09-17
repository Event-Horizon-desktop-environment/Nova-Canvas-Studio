#include "timeline_volume_line.hpp"

#include "canvas/core/timeline/audio_mix.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::gui::timeline_volume_line {

namespace {

double keep_positive(double d) {
    return std::max(1.0, d);
}

}

double volume_line_y(double db, double body_h) {
    using namespace canvas::core::audio_mix;
    const double c =
        std::clamp(db, static_cast<double>(kMinVolumeDb),
                   static_cast<double>(kMaxVolumeDb));
    const double lo = std::min(kVolumeLinePad, body_h);
    const double hi = std::min(body_h, std::max(kVolumeLinePad, body_h - kVolumeLinePad));
    const double center = std::clamp(body_h / 2.0, lo, hi);
    if (c >= 0.0) {
        const double t = c / static_cast<double>(kMaxVolumeDb);
        const double span = keep_positive(center - lo);
        return std::clamp(center - t * span, lo, hi);
    }
    const double t = -c / -static_cast<double>(kMinVolumeDb);
    const double span = keep_positive(hi - center);
    return std::clamp(center + t * span, lo, hi);
}

double db_from_volume_line_y(double y, double body_h) {
    using namespace canvas::core::audio_mix;
    const double lo = std::min(kVolumeLinePad, body_h);
    const double hi = std::min(body_h, std::max(kVolumeLinePad, body_h - kVolumeLinePad));
    const double center = std::clamp(body_h / 2.0, lo, hi);
    const double clamped_y = std::clamp(y, lo, hi);
    if (clamped_y <= center) {
        const double t = (center - clamped_y) / keep_positive(center - lo);
        return kMaxVolumeDb * t;
    }
    const double t = (clamped_y - center) / keep_positive(hi - center);
    return kMinVolumeDb * t;
}

double drag_db_from_relative_y(double rel_y, double body_h, double curve) {
    const double lo = std::min(kVolumeLinePad, body_h);
    const double hi = std::min(body_h, std::max(kVolumeLinePad, body_h - kVolumeLinePad));
    const double center = std::clamp(body_h / 2.0, lo, hi);
    const double e = keep_positive(curve);
    const double top_span = keep_positive(center - lo);
    const double bottom_span = keep_positive(hi - center);
    double effective = rel_y;
    if (rel_y <= center) {
        const double u = (center - rel_y) / top_span;
        effective = center - std::pow(u, e) * top_span;
    } else {
        const double u = (rel_y - center) / bottom_span;
        effective = center + std::pow(u, e) * bottom_span;
    }
    return db_from_volume_line_y(effective, body_h);
}

double volume_waveform_scale(double db) {
    using namespace canvas::core::audio_mix;
    const double c = std::clamp(db, static_cast<double>(kMinVolumeDb),
                                static_cast<double>(kMaxVolumeDb));
    const double f = (c - static_cast<double>(kMinVolumeDb)) / -static_cast<double>(kMinVolumeDb);
    const double s = kVolumeWaveformScaleFloor +
                     (1.0 - kVolumeWaveformScaleFloor) * std::pow(f, 1.5);
    return std::clamp(s, 0.0, 1.0);
}

}
