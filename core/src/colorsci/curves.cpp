#include "canvas/core/colorsci/curves.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace canvas::core::colorsci {

namespace {

constexpr float kEps = 1.0e-6f;

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

}

float eval_curve(const std::vector<CurvePoint>& points, float x) {
    if (points.empty()) return x;

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

    if (knots.size() < 2) return x;

    const std::size_t n = knots.size();
    const float xi = std::clamp(x, 0.0f, 1.0f);

    std::vector<float> d(n - 1, 0.0f);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const float dx = knots[i + 1].x - knots[i].x;
        if (dx > kEps) d[i] = (knots[i + 1].y - knots[i].y) / dx;
    }

    std::vector<float> m(n, 0.0f);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if ((d[i - 1] > 0.0f && d[i] < 0.0f) || (d[i - 1] < 0.0f && d[i] > 0.0f) ||
            d[i - 1] == 0.0f || d[i] == 0.0f) {
            m[i] = 0.0f;
        } else {
            m[i] = 0.5f * (d[i - 1] + d[i]);
        }
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0f) {
            m[i] = 0.0f;
            m[i + 1] = 0.0f;
            continue;
        }
        const float alpha = m[i] / d[i];
        const float beta = m[i + 1] / d[i];
        const float sq = alpha * alpha + beta * beta;
        if (sq > 9.0f) {
            const float tau = 3.0f / std::sqrt(sq);
            m[i] *= tau;
            m[i + 1] *= tau;
        }
    }

    for (std::size_t i = 0; i + 1 < n; ++i) {
        const float x0 = knots[i].x;
        const float x1 = knots[i + 1].x;
        if (xi < x1 || (xi == 1.0f && x1 == 1.0f)) {
            if (x1 - x0 <= kEps) continue;
            const float t = std::clamp((xi - x0) / (x1 - x0), 0.0f, 1.0f);
            return hermite(t, knots[i].y, m[i], knots[i + 1].y, m[i + 1], x1 - x0);
        }
    }

    float best_y = 0.0f;
    float best_d = std::numeric_limits<float>::max();
    for (const CurvePoint& k : knots) {
        const float dd = std::abs(k.x - xi);
        if (dd < best_d) {
            best_d = dd;
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
    const auto& red_pts = c.channels[static_cast<std::size_t>(CurveChannel::kRed)];
    const auto& grn_pts = c.channels[static_cast<std::size_t>(CurveChannel::kGreen)];
    const auto& blu_pts = c.channels[static_cast<std::size_t>(CurveChannel::kBlue)];
    const bool r_edit = !red_pts.empty();
    const bool g_edit = !grn_pts.empty();
    const bool b_edit = !blu_pts.empty();
    const int edited = static_cast<int>(r_edit) + static_cast<int>(g_edit) +
                       static_cast<int>(b_edit);

    float r = eval_curve(red_pts, rgb.r);
    float g = eval_curve(grn_pts, rgb.g);
    float b = eval_curve(blu_pts, rgb.b);

    if (edited == 1) {
        const float L_in = kLuma601R * rgb.r + kLuma601G * rgb.g + kLuma601B * rgb.b;
        const float wp = r_edit ? kLuma601R : (g_edit ? kLuma601G : kLuma601B);
        const float w_oa = r_edit ? kLuma601G : (g_edit ? kLuma601R : kLuma601R);
        const float w_ob = r_edit ? kLuma601B : (g_edit ? kLuma601B : kLuma601G);
        const float in_oa = r_edit ? rgb.g : (g_edit ? rgb.r : rgb.r);
        const float in_ob = r_edit ? rgb.b : (g_edit ? rgb.b : rgb.g);

        float primary = r_edit ? r : (g_edit ? g : b);
        const float den = w_oa * in_oa + w_ob * in_ob;
        const float excess = L_in - wp * primary;
        float k = den > kEps ? excess / den : 0.0f;
        const float cap = std::max(in_oa, in_ob);
        if (cap > kEps) k = std::min(k, 1.0f / cap);
        k = std::max(k, 0.0f);
        float out_oa = k * in_oa;
        float out_ob = k * in_ob;
        const float err = L_in - (wp * primary + w_oa * out_oa + w_ob * out_ob);
        if (std::fabs(err) > kEps && wp > kEps) {
            primary = std::clamp(primary + err / wp, 0.0f, 1.0f);
        }
        if (r_edit) {
            r = primary;
            g = out_oa;
            b = out_ob;
        } else if (g_edit) {
            r = out_oa;
            g = primary;
            b = out_ob;
        } else {
            r = out_oa;
            g = out_ob;
            b = primary;
        }
    }

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

}
