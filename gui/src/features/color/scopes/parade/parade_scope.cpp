#include "features/color/scopes/parade/parade_scope.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace canvas::gui {

namespace {
constexpr int kParadeCols = 3 * kScopeCols;  // three touching thirds (spec §3: no gap)
}  // namespace

void ParadeScope::recompute_render() {
    hist_.clear();
    const auto& frame = cached();
    if (!frame) return;
    if (frame->a && !frame->a->rgba.empty()) {
        hist_.accumulate(*frame->a);
    } else if (frame->nv12 && !frame->nv12->y.empty()) {
        hist_.accumulate(*frame->nv12);
    }

    content_ = QImage(kParadeCols, kScopeLevels, QImage::Format_ARGB32_Premultiplied);
    content_.fill(QColor(3, 4, 7));

    const auto& h = hist_.channels();
    for (int ch = 0; ch < 3; ++ch) {
        // Per-channel normalization (spec §2 step 4): log-scaled density so a
        // single stray bright pixel is a faint dot while a solid region glows.
        std::uint32_t max_count = 1;
        for (int i = 0; i < kScopeCellCount; ++i) {
            max_count = std::max(max_count, h[std::size_t(ch) * kScopeCellCount + i]);
        }
        const double log_max = std::log1p(static_cast<double>(max_count));

        const QColor& col = kScopeWaveRgb[ch];
        for (int x = 0; x < kScopeCols; ++x) {
            const std::size_t base = std::size_t(x) * kScopeLevels;
            for (int level = 0; level < kScopeLevels; ++level) {
                const std::uint32_t n = h[std::size_t(ch) * kScopeCellCount + base + level];
                if (n == 0) continue;
                const double a = density_alpha(n, log_max);
                const std::size_t qa = static_cast<std::size_t>(a * 255.0 + 0.5);
                const std::size_t row = std::size_t(kScopeLevels - 1 - level);
                uint32_t* dst_row = reinterpret_cast<uint32_t*>(content_.scanLine(int(row)));
                dst_row[std::size_t(ch) * kScopeCols + x] =
                    (qa << 24) |
                    ((col.red() * qa / 255) << 16) |
                    ((col.green() * qa / 255) << 8) |
                    ((col.blue() * qa / 255));
            }
        }
    }
}

void ParadeScope::paint_body(QPainter& p, const QRectF& plot) {
    if (!content_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(plot, content_);
    }
    paint_column_grid(p, plot, /*channel_split=*/true);
}

}  // namespace canvas::gui