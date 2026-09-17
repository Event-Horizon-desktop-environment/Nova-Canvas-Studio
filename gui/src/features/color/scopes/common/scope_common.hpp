#pragma once

#include <QWidget>

#include <array>
#include <cstdint>

#include "canvas/core/colorsci/histogram.hpp"
#include "canvas/core/media/frame.hpp"

class QImage;
class QPainter;

namespace canvas::gui {

enum class ScopeMode { Waveform, Parade, Vectorscope, Histogram, CIE };

enum class ScopeDisplay { Luma, Rgb, Yrgb };

using ColumnHistogram = canvas::core::colorsci::ColumnHistogram;
inline constexpr int kScopeCols = canvas::core::colorsci::kHistogramCols;
inline constexpr int kScopeLevels = canvas::core::colorsci::kHistogramLevels;
inline constexpr int kScopeCellCount = canvas::core::colorsci::kHistogramCellCount;
inline constexpr int kScopeTargetSamples = canvas::core::colorsci::kHistogramTargetSamples;
constexpr int kScopeVec = 256;

const QColor kScopeWaveRgb[3] = {QColor(0xFF, 0x33, 0x33), QColor(0x4C, 0xFF, 0x4C),
                                 QColor(0x55, 0x8A, 0xFF)};
const QColor kScopeLumaWhite = QColor(0xE8, 0xE8, 0xE6);

class ScopePane : public QWidget {
public:
    explicit ScopePane(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 160);
    }

    void update_frame(canvas::core::RenderFramePtr frame);

protected:
    void paintEvent(QPaintEvent* event) override;

    virtual void recompute_render() = 0;
    virtual void paint_body(QPainter& p, const QRectF& plot) = 0;

    [[nodiscard]] const canvas::core::RenderFramePtr& cached() const { return cached_; }

private:
    canvas::core::RenderFramePtr cached_;
};

double density_alpha(std::uint32_t count, double log_max);

QImage log_density_plane(const std::uint32_t* hist, int cols, int levels, const QColor& color);

QImage log_bar_plane(const std::uint32_t* counts, int levels, const QColor& color);

QRectF paint_scope_chrome(QPainter& p, const QWidget& host);

void paint_column_grid(QPainter& p, const QRectF& plot, bool channel_split);

}
