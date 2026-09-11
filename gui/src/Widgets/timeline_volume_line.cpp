#include "timeline_volume_line.hpp"

#include "canvas/core/timeline/audio_mix.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::gui::timeline_volume_line {

namespace {

// Usable travel between the padded edges; the two padding points can't overlap
// when the box is tiny (degenerate 1px boxes still map sanely).
double keep_positive(double d) {
    return std::max(1.0, d);
}

}  // namespace

double volume_line_y(double db, double body_h) {
    using namespace canvas::core::audio_mix;
    const double c =
        std::clamp(db, static_cast<double>(kMinVolumeDb),
                   static_cast<double>(kMaxVolumeDb));
    const double lo = std::min(kVolumeLinePad, body_h);
    const double hi = std::min(body_h, std::max(kVolumeLinePad, body_h - kVolumeLinePad));
    const double center = std::clamp(body_h / 2.0, lo, hi);
    if (c >= 0.0) {
        // 0 dB center -> +24 dB top edge: half the travel, +amplitude upward.
        const double t = c / static_cast<double>(kMaxVolumeDb);
        const double span = keep_positive(center - lo);
        return std::clamp(center - t * span, lo, hi);
    }
    // 0 dB center -> -100 dB bottom edge (silence, db_to_gain -> 0): the other
    // half of the travel carries the full mute range so the very bottom of the
    // clip IS digital silence.
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
    // Raise the fractional distance from the center to `curve` (>=1) before the
    // dB is read: small moves around the 0 dB center yield small dB swings
    // (fine-grained, slow), while the box edges still map to the band limits
    // EXACTLY (|1|^curve == 1), so a full box-height gesture still reaches the
    // very bottom/top of the clip. Positions beyond the box keep driving the
    // value to the law band limits (clamped by db_from_volume_line_y).
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
    // Fraction of the down-travel band (0 = silence at -100, 1 = 0 dB): using the
    // band fraction instead of raw linear gain keeps the scale monotonically
    // responsive across the WHOLE band — raw db_to_gain collapses to ~0 for
    // everything under -20 dB, which would make 90% of a volume drag visually
    // inert. The floor keeps the spectrum visible (never a flat line) while
    // staying monotone: quieter = shorter bars, louder = fuller bars, all the
    // way down to -100 dB.
    const double c = std::clamp(db, static_cast<double>(kMinVolumeDb),
                                static_cast<double>(kMaxVolumeDb));
    const double f = (c - static_cast<double>(kMinVolumeDb)) / -static_cast<double>(kMinVolumeDb);
    const double s = kVolumeWaveformScaleFloor +
                     (1.0 - kVolumeWaveformScaleFloor) * std::pow(f, 1.5);
    return std::clamp(s, 0.0, 1.0);
}

}  // namespace canvas::gui::timeline_volume_line