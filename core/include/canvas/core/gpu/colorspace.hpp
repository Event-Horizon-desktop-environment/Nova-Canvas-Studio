#pragma once

// Color space conversion helpers shared by the preview viewer (GPU NV12 upload +
// CPU NV12->RGBA fallback) and the export/NVENC path. Keeping the coefficients and
// the pixel math in ONE place means the two paths can never drift (they previously
// duplicated the BT.601 limited-range constants).
//
// BT.709 limited range: Y in [16,235], Cb/Cr in [16,240] centered on 128.
// RGB out is per-pixel, clamped to [0,255].
//
// This header is deliberately Qt-free so it is usable from both the GUI viewer and
// the headless export engine. It is also included by cuda_convert.cu.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace canvas::core::gpu {

namespace bt709 {
// YUV -> RGB (BT.709 limited range) coefficients and offsets. The viewer's CPU
// fallback and the GPU shaders (viewer_gl.cpp) must agree on these exactly.
// This is the source content's actual color space — see exporter.cpp's
// color_primaries/colorspace tags. Previously this used BT.601 coefficients,
// which caused a magenta/purple skin-tone shift on BT.709 sources (i.e. all
// modern webcam/screen capture).
inline constexpr float kYOffset = 16.0f;   // black-level Y
inline constexpr float kCbCrCenter = 128.0f;
inline constexpr float kRY = 1.164f;
inline constexpr float kRCr = 1.793f;
inline constexpr float kGY = 1.164f;
inline constexpr float kGCb = -0.213f;
inline constexpr float kGCr = -0.533f;
inline constexpr float kBY = 1.164f;
inline constexpr float kBCb = 2.112f;
}  // namespace bt709
// Kept as an alias so any remaining bt601:: references still compile while
// the rest of the codebase (histogram.cpp, vectorscope) is migrated off the
// old name — remove once nothing references bt601:: directly.
namespace bt601 = bt709;

// Converts a single BT.709 limited-range pixel. All inputs are the raw 8-bit YUV
// values (Y, Cb, Cr in [0,255]); returns the packed 0xRRGGBB-ish ints via out r,g,b
// references clamped to [0,255]. This is the canonical form both the CPU viewer path
// and (via struct layout twin) the CUDA kernel implement.
struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

inline Rgb8 yuv_to_rgb(const std::uint8_t y, const std::uint8_t cb, const std::uint8_t cr) {
    using namespace bt601;
    const float Y = static_cast<float>(y);
    const float Cb = static_cast<float>(cb) - kCbCrCenter;
    const float Cr = static_cast<float>(cr) - kCbCrCenter;
    const float yr = Y - kYOffset;
    // Round-to-nearest then clamp to [0,255], preserving the viewer's original
    // (llround-based) CPU fallback exactly so the GPU and CPU paths agree bit-for-bit.
    Rgb8 out;
    out.r = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::llround(yr * kRY + Cr * kRCr)), 0, 255));
    out.g = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::llround(yr * kGY + Cb * kGCb + Cr * kGCr)), 0, 255));
    out.b = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::llround(yr * kBY + Cb * kBCb)), 0, 255));
    return out;
}

namespace bt709_rgb2yuv {
// RGB -> YUV (BT.709 limited range) coefficients, the inverse of the yuv_to_rgb
// path above. This is the reference for the CUDA kernel (cuda_convert.cu),
// which implements the same math on-device; keeping the constants here is the
// single source of truth for the export color path. Y in [16,235], Cb/Cr in
// [16,240]. Matches exporter.cpp's BT.709 stream tags — was previously BT.601,
// which mismatched the tagged output and reproduced the same purple skin-tone
// shift on playback of exported files.
inline constexpr float kY_R = 0.183f, kY_G = 0.614f, kY_B = 0.062f;
inline constexpr float kCb_R = -0.101f, kCb_G = -0.339f, kCb_B = 0.439f;
inline constexpr float kCr_R = 0.439f, kCr_G = -0.399f, kCr_B = -0.040f;
inline constexpr std::uint8_t kYMin = 16, kYMax = 235;
inline constexpr std::uint8_t kCMin = 16, kCMax = 240;
}  // namespace bt709_rgb2yuv
namespace bt601_rgb2yuv = bt709_rgb2yuv;

// RGB -> YUV reference (BT.709 limited range). CPU reference for the CUDA path;
// matches cuda_convert.cu's rgbaToNV12 kernel coefficient-for-coefficient.
struct Yuv8 {
    std::uint8_t y = 0;
    std::uint8_t cb = 0;
    std::uint8_t cr = 0;
};

inline Yuv8 rgb_to_yuv(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b) {
    using namespace bt601_rgb2yuv;
    const float rf = static_cast<float>(r), gf = static_cast<float>(g), bf = static_cast<float>(b);
    Yuv8 out;
    out.y = static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::llround(16.0f + kY_R * rf + kY_G * gf + kY_B * bf)),
        static_cast<int>(kYMin), static_cast<int>(kYMax)));
    out.cb = static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::llround(128.0f + kCb_R * rf + kCb_G * gf + kCb_B * bf)),
        static_cast<int>(kCMin), static_cast<int>(kCMax)));
    out.cr = static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::llround(128.0f + kCr_R * rf + kCr_G * gf + kCr_B * bf)),
        static_cast<int>(kCMin), static_cast<int>(kCMax)));
    return out;
}

// ---- Per-frame color specification (threaded from the decoder) -------------
//
// A frame's raw YUV planes carry two independent properties: the YUV->RGB
// coefficient set (`matrix`: 601 / 709 / 2020) and the quantization (`range`:
// limited-TV luma 16..235/chroma 16..240 vs full-JPEG 0..255). Every consumer
// (GPU shader, CPU fallback, scopes, swscale, export) must pick BOTH from the
// file — a limited-only assumption is exactly the 601-vs-709 matrix mismatch
// and the limited-arithmetic-on-full-data lavenders this repo has fought.
//
// The decoder reads the tags from codecpar and reconciles them with a luma
// range probe (OBS-family files stamp `tv` while writing full-range data), so
// `ColorSpec` carries the RESOLVED values the file actually uses, not the raw
// tags. RGB frames (VideoFrame) need no spec — they are already full-range RGB.
enum class ColorMatrix : std::uint8_t { BT601 = 0, BT709 = 1, BT2020 = 2 };
enum class ColorRange : std::uint8_t { Limited = 0, Full = 1 };

inline constexpr const char* color_matrix_name(const ColorMatrix m) noexcept {
    switch (m) {
        case ColorMatrix::BT601: return "bt601";
        case ColorMatrix::BT2020: return "bt2020";
        default: return "bt709";
    }
}
inline constexpr const char* color_range_name(const ColorRange r) noexcept {
    return r == ColorRange::Full ? "full" : "limited";
}

struct ColorSpec {
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::Limited;
};

// YUV -> RGB chroma gains in the normalized (full-swing) form where equalized
// luma is added 1:1: R = Y + r_cr*Cr, G = Y + g_cb*Cb + g_cr*Cr, B = Y + b_cb*Cb.
// The `*Ltd` variants fold in the 255/224 chroma headroom so a limited-range
// decode needs no extra scaling. These literals are mirrored 1:1 in the viewer
// shaders (kFragNv12Src / kFragNv12Trans) so the CPU fallback and the GPU path
// agree on every (matrix, range) pair.
struct MatrixCoeffs {
    float r_cr, g_cb, g_cr, b_cb;
};
inline constexpr MatrixCoeffs kMatFullBT601{1.402f, -0.344f, -0.714f, 1.772f};
inline constexpr MatrixCoeffs kMatLtdBT601{1.596f, -0.392f, -0.813f, 2.017f};
inline constexpr MatrixCoeffs kMatFullBT709{1.5748f, -0.1873f, -0.4681f, 1.8556f};
inline constexpr MatrixCoeffs kMatLtdBT709{1.793f, -0.213f, -0.533f, 2.112f};  // == legacy constants
inline constexpr MatrixCoeffs kMatFullBT2020{1.4746f, -0.1645f, -0.5714f, 1.8814f};
inline constexpr MatrixCoeffs kMatLtdBT2020{1.679f, -0.187f, -0.650f, 2.142f};
// Limited-range luma needs the 16..235 elementary headroom unwound by 255/219.
inline constexpr float kLimitedLumaScale = 1.164f;

inline MatrixCoeffs matrix_coeffs(const ColorMatrix m, const ColorRange r) {
    const bool ltd = r == ColorRange::Limited;
    switch (m) {
        case ColorMatrix::BT601: return ltd ? kMatLtdBT601 : kMatFullBT601;
        case ColorMatrix::BT2020: return ltd ? kMatLtdBT2020 : kMatFullBT2020;
        default: return ltd ? kMatLtdBT709 : kMatFullBT709;
    }
}

// Range- and matrix-aware YUV -> RGB. The (Limited, BT709) combo is the legacy
// <4-arg> default, preserved bit-for-bit (same llround/rounding law, same
// constants) so existing tests and the old CPU/GPU agreement hold. All other
// combos use the coefficient table above; the shaders mirror it exactly.
inline Rgb8 yuv_to_rgb(const std::uint8_t y, const std::uint8_t cb, const std::uint8_t cr,
                       const ColorRange range, const ColorMatrix matrix) {
    if (range == ColorRange::Limited && matrix == ColorMatrix::BT709)
        return yuv_to_rgb(y, cb, cr);
    const MatrixCoeffs k = matrix_coeffs(matrix, range);
    const float Y = (range == ColorRange::Limited) ? (static_cast<float>(y) - 16.0f) * kLimitedLumaScale
                                                    : static_cast<float>(y);
    const float Cb = static_cast<float>(cb) - 128.0f;
    const float Cr = static_cast<float>(cr) - 128.0f;
    const auto clamp8 = [](int v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    };
    Rgb8 out;
    out.r = clamp8(static_cast<int>(std::llround(Y + k.r_cr * Cr)));
    out.g = clamp8(static_cast<int>(std::llround(Y + k.g_cb * Cb + k.g_cr * Cr)));
    out.b = clamp8(static_cast<int>(std::llround(Y + k.b_cb * Cb)));
    return out;
}

}  // namespace canvas::core::gpu
