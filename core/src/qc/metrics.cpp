#include "canvas/core/qc/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace canvas::core::qc {

namespace {

constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

std::uint8_t at(const Plane& p, const int x, const int y) noexcept {
    return p.data[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.stride) +
                  static_cast<std::size_t>(x)];
}

}

bool valid(const Plane& p) noexcept {
    return p.data != nullptr && p.width > 0 && p.height > 0 && p.stride >= p.width;
}

double psnr(const Plane& a, const Plane& b) noexcept {
    if (!valid(a) || !valid(b) || a.width != b.width || a.height != b.height) return 0.0;

    long double sse = 0.0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const double d = static_cast<double>(at(a, x, y)) - static_cast<double>(at(b, x, y));
            sse += d * d;
        }
    }
    if (sse == 0.0) return std::numeric_limits<double>::infinity();
    const double mse = static_cast<double>(sse) / (static_cast<double>(a.width) * a.height);
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double ssim(const Plane& a, const Plane& b) noexcept {
    if (!valid(a) || !valid(b) || a.width != b.width || a.height != b.height) return 0.0;

    const int win_w = std::min(8, a.width);
    const int win_h = std::min(8, a.height);
    const int step_x = std::max(1, win_w / 2);
    const int step_y = std::max(1, win_h / 2);

    double total = 0.0;
    int windows = 0;
    for (int oy = 0; oy + win_h <= a.height; oy += step_y) {
        for (int ox = 0; ox + win_w <= a.width; ox += step_x) {
            const double n = static_cast<double>(win_w) * win_h;
            double sum_x = 0.0, sum_y = 0.0;
            for (int y = 0; y < win_h; ++y) {
                for (int x = 0; x < win_w; ++x) {
                    sum_x += at(a, ox + x, oy + y);
                    sum_y += at(b, ox + x, oy + y);
                }
            }
            const double mean_x = sum_x / n;
            const double mean_y = sum_y / n;

            double var_x = 0.0, var_y = 0.0, cov = 0.0;
            for (int y = 0; y < win_h; ++y) {
                for (int x = 0; x < win_w; ++x) {
                    const double dx = at(a, ox + x, oy + y) - mean_x;
                    const double dy = at(b, ox + x, oy + y) - mean_y;
                    var_x += dx * dx;
                    var_y += dy * dy;
                    cov += dx * dy;
                }
            }
            var_x /= n;
            var_y /= n;
            cov /= n;

            const double num =
                (2.0 * mean_x * mean_y + kC1) * (2.0 * cov + kC2);
            const double den =
                (mean_x * mean_x + mean_y * mean_y + kC1) * (var_x + var_y + kC2);
            total += num / den;
            ++windows;
        }
    }
    return windows > 0 ? total / windows : 0.0;
}

}
