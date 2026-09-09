#pragma once

// Shared plumbing for the real video scopes (rgb-parade-scope-implementation-
// spec.md §4 + waveform-vectorscope-histogram-cie-implementation-spec.md §5).
// Each scope lives in its own folder with one class per file; what they share —
// mode/display enums, the frame-caching painted shell, the stride-sampled
// column/level accumulation and its log-normalized density rendering — lives
// here so a scope file stays a pure "what is this scope" description.

#include <QWidget>

#include <array>
#include <cstdint>

#include "canvas/core/media/frame.hpp"

class QImage;
class QPainter;

namespace canvas::gui {

// Scope selectable from the Scopes panel dropdown. Values stay stable: the
// dropdown item order and this enum are wired by index in ScopesPanel (the
// stacked page index equals the enum value).
enum class ScopeMode { Waveform, Parade, Vectorscope, Histogram, CIE };

// Sub-display variant for scopes that have one (panel sub-dropdown).
enum class ScopeDisplay { Luma, Rgb, Yrgb };

// Column buckets + 256 level buckets per channel (parade spec §2 step 2: 300–500
// columns; 256 levels is visually indistinguishable from 1024 for 8-bit input).
constexpr int kScopeCols = 384;
constexpr int kScopeLevels = 256;
constexpr int kScopeCellCount = kScopeCols * kScopeLevels;
// Cb×Cr scatter resolution for the vectorscope (wave spec §2 step 2).
constexpr int kScopeVec = 256;
// Bounded CPU sample budget per frame (parade spec §2 step 3: a full-res
// every-pixel scatter would not keep up at playback; stride sampling keeps the
// density shape intact and the cost well under the frame budget).
constexpr int kScopeTargetSamples = 262144;

// Channel colors shared by Parade and the Waveform RGB/YRGB overlays.
const QColor kScopeWaveRgb[3] = {QColor(0xFF, 0x33, 0x33), QColor(0x4C, 0xFF, 0x4C),
                                 QColor(0x55, 0x8A, 0xFF)};
const QColor kScopeLumaWhite = QColor(0xE8, 0xE8, 0xE6);

// Stride-sampled per-frame column/level accumulation, shared by Parade,
// Waveform, and Histogram (the histogram is a reduction of the same buffers at
// render time — wave spec §3: never a second read of the frame).
class ColumnHistogram {
public:
    void clear();
    void accumulate(const canvas::core::VideoFrame& rgba);
    void accumulate(const canvas::core::Nv12Frame& nv12);

    [[nodiscard]] const std::array<std::uint32_t, 3 * kScopeCellCount>& channels() const {
        return hist_;
    }
    [[nodiscard]] const std::array<std::uint32_t, kScopeCellCount>& luma() const {
        return hist_luma_;
    }

private:
    std::array<std::uint32_t, 3 * kScopeCellCount> hist_{};
    std::array<std::uint32_t, kScopeCellCount> hist_luma_{};
};

// Painted shell for every live scope: caches the last presented RenderFrame,
// re-runs the virtual accumulation on a NEW pointer only, and unifies the panel
// chrome + plot clipping in one paintEvent. Subclasses are pure functions of
// "what to accumulate" and "how to draw it" — nothing here recomputes on paint.
class ScopePane : public QWidget {
public:
    explicit ScopePane(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 160);
    }

    // Feed the frame the preview viewer just presented. Only a NEW shared
    // pointer triggers a recompute; identical pointers reuse the cache.
    void update_frame(canvas::core::RenderFramePtr frame);

protected:
    void paintEvent(QPaintEvent* event) override;

    virtual void recompute_render() = 0;
    virtual void paint_body(QPainter& p, const QRectF& plot) = 0;

    [[nodiscard]] const canvas::core::RenderFramePtr& cached() const { return cached_; }

private:
    canvas::core::RenderFramePtr cached_;
};

// log1p-normalized, single-color, premultiplied density plane for `cols`
// Density→alpha law shared by the scatter planes (both waveforms + parade):
// log1p-scaled density, then a sub-linear boost (`pow(x, 0.6)`) so sparse
// traces stay visible while dense cores drive straight to full opacity —
// matches the bright, hot-core character of a Resolve/ScopeBox trace instead
// of a straight `log1p(n)/log_max` mapping that barely lifts sparse cells.
double density_alpha(std::uint32_t count, double log_max);

// columns × `levels` level buckets. Cell (col, level) holds hist[col*levels +
// level]. Rows are drawn top-down (row 0 == level 255 == top of the plot).
QImage log_density_plane(const std::uint32_t* hist, int cols, int levels, const QColor& color);

// Solid bars for a 1D level histogram: one column per level, filled from the
// bottom up to a log1p-normalized height. Premultiplied so overlaid channels
// can be combined additively later.
QImage log_bar_plane(const std::uint32_t* counts, int levels, const QColor& color);

// Rounded panel chrome + dark plot field; returns the plot rect to paint in.
QRectF paint_scope_chrome(QPainter& p, const QWidget& host);

// 8-division gridline overlay with 0..1023 labels on the far-left edge
// (parade spec §3); `channel_split` adds the two hairline parade separators.
void paint_column_grid(QPainter& p, const QRectF& plot, bool channel_split);

}  // namespace canvas::gui