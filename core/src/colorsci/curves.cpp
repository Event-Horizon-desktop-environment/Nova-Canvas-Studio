// Custom-curve grade law — evaluation of the spline, the soft-clip rails, and
// the combined per-channel/luma/soft-clip pass. Header `curves.hpp` documents
// the model; this file is the math. Qt-free, tests in core/tests/curves_test.

#include "canvas/core/colorsci/curves.hpp"

#include <algorithm>
#include <limits>

namespace canvas::core::colorsci {

namespace {

constexpr float kEps = 1.0e-6f;

// Hermite basis for a Catmull-Rom segment. Tangents are finite-difference
// slopes clamped at the endpoints (standard Catmull-Rom with clamped tangents
// — the "clamped spline" form; see header for why).
[[nodiscard]] inline float hermite(float t, float p1, float m1, float p2, float m2,
                                   float seg_len) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * p1 + h10 * m1 * seg_len + h01 * p2 + h11 * m2 * seg_len;
}

}  // namespace

float eval_curve(const std::vector<CurvePoint>& points, float x) {
    // Empty == identity channel: straight line from (0,0) to (1,1).
    if (points.empty()) return x;

    // Build the full knot list: implicit endpoints + interior points, sorted
    // by x. Duplicate x values keep the LAST point (drag-on-top wins), which
    // collapses vertical stems into a single knot — a duplicate x would
    // otherwise produce a zero-length segment and a division blow-up below.
    std::vector<CurvePoint> knots;
    knots.reserve(points.size() + 2);
    knots.push_back(CurvePoint{0.0f, 0.0f});
    for (const CurvePoint& p : points) {
        knots.push_back(CurvePoint{std::clamp(p.x, 0.0f, 1.0f), std::clamp(p.y, 0.0f, 1.0f)});
    }
    knots.push_back(CurvePoint{1.0f, 1.0f});
    std::stable_sort(knots.begin(), knots.end(),
                     [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    {
        // Collapse equal-x runs keeping the last (topmost) point.
        std::vector<CurvePoint> merged;
        merged.reserve(knots.size());
        for (const CurvePoint& k : knots) {
            if (merged.empty() ||
                std::fabs(merged.back().x - k.x) > std::numeric_limits<float>::epsilon()) {
                merged.push_back(k);
            } else {
                merged.back() = k;
            }
        }
        knots = std::move(merged);
    }

    // A curve that collapsed to nothing (all points at the corners) behaves
    // as identity.
    if (knots.size() < 2) return x;

    const float xi = std::clamp(x, 0.0f, 1.0f);

    // Rightmost segment takes x == 1; otherwise the segment where xi is in
    // [k[i].x, k[i+1].x).
    for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
        const float x0 = knots[i].x;
        const float x1 = knots[i + 1].x;
        if (xi < x1 || (xi == 1.0f && x1 == 1.0f)) {
            if (x1 - x0 <= kEps) continue;  // pathological adjacent duplicates: fall through
            const float t = std::clamp((xi - x0) / (x1 - x0), 0.0f, 1.0f);

            const auto& preced = i == 0 ? knots[i] : knots[i - 1];
            const auto& follow = i + 2 >= knots.size() ? knots[i + 1] : knots[i + 2];
            const float m1 = (knots[i + 1].y - preced.y) /
                             std::max(preced.x != knots[i + 1].x ? knots[i + 1].x - preced.x
                                                                 : 1.0f,
                                      kEps);
            // Endpoint tangents clamp: no tangent can drive the curve outside
            // the extreme ladder direction beyond its neighbour knot.
            const float m2 = (follow.y - knots[i].y) /
                             std::max(follow.x != knots[i].x ? follow.x - knots[i].x : 1.0f,
                                      kEps);
            return hermite(t, knots[i].y, m1, knots[i + 1].y, m2, x1 - x0);
        }
    }

    // Degenerate fallback: an interior x exactly on a duplicated interior knot
    // falls between two candidate segments only after dedupe collapses them;
    // snap to the nearest knot.
    float best_y = 0.0f;
    float best_d = std::numeric_limits<float>::max();
    for (const CurvePoint& k : knots) {
        const float d = std::abs(k.x - xi);
        if (d < best_d) {
            best_d = d;
            best_y = k.y;
        }
    }
    return best_y;
}

float eval_soft_clip_high(float x, float high, float soft) noexcept {
    const float h = std::clamp(high, 0.0f, 1.0f);
    if (h >= 1.0f || soft <= 0.0f || x <= h) return x;
    const float s = std::clamp(soft, 0.0f, 1.0f);
    const float tau = std::clamp((x - h) / std::max(1.0f - h, kEps), 0.0f, 1.0f);
    // Folded excess over the rail: tau * (1 - s*ss(tau)). ss is the cubic
    // smoothstep so the fold is monotone and C1 at the rail (slope 1 both
    // sides); soft=1 makes out(1) == h (crush onto the rail).
    const float ss = tau * tau * (3.0f - 2.0f * tau);
    return h + (1.0f - h) * tau * (1.0f - s * ss);
}

float eval_soft_clip_low(float x, float low, float soft) noexcept {
    const float lo = std::clamp(low, 0.0f, 1.0f);
    if (lo <= 0.0f || soft <= 0.0f || x >= lo) return x;
    const float s = std::clamp(soft, 0.0f, 1.0f);
    const float tau = std::clamp((lo - x) / std::max(lo, kEps), 0.0f, 1.0f);
    const float ss = tau * tau * (3.0f - 2.0f * tau);
    return lo - lo * tau * (1.0f - s * ss);
}

RGBF apply_curves(const RGBF& rgb, const CurveParams& c) noexcept {
    // Channel curves first: independent R/G/B reshaping.
    const float r = eval_curve(c.channels[static_cast<int>(CurveChannel::kRed)], rgb.r);
    const float g = eval_curve(c.channels[static_cast<int>(CurveChannel::kGreen)], rgb.g);
    const float b = eval_curve(c.channels[static_cast<int>(CurveChannel::kBlue)], rgb.b);

    // Luma curve: reshape the luma, then hue-preservingly rescale each channel
    // by the luma ratio. Dark luma (no hue to preserve) skips the ratio.
    float L = kLuma601R * r + kLuma601G * g + kLuma601B * b;
    const float Lc = eval_curve(c.channels[static_cast<int>(CurveChannel::kLuma)], L);
    float r2 = r;
    float g2 = g;
    float b2 = b;
    if (L > kEps) {
        const float s = Lc / L;
        r2 = r * s;
        g2 = g * s;
        b2 = b * s;
    }

    // Soft-clip toe then shoulder on the graded luma, again ratio-scaled.
    float Ls = eval_soft_clip_low(Lc, c.soft_clip.low, c.soft_clip.low_soft);
    Ls = eval_soft_clip_high(Ls, c.soft_clip.high, c.soft_clip.high_soft);
    if (Lc > kEps && Ls != Lc) {
        const float s2 = Ls / Lc;
        r2 *= s2;
        g2 *= s2;
        b2 *= s2;
    }

    return RGBF{std::clamp(r2, 0.0f, 1.0f), std::clamp(g2, 0.0f, 1.0f),
                std::clamp(b2, 0.0f, 1.0f)};
}

}  // namespace canvas::core::colorsci