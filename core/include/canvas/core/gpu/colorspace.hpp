#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace canvas::core::gpu {

namespace bt709 {
inline constexpr float kYOffset = 16.0f;
inline constexpr float kCbCrCenter = 128.0f;
inline constexpr float kRY = 1.164f;
inline constexpr float kRCr = 1.793f;
inline constexpr float kGY = 1.164f;
inline constexpr float kGCb = -0.213f;
inline constexpr float kGCr = -0.533f;
inline constexpr float kBY = 1.164f;
inline constexpr float kBCb = 2.112f;
}
namespace bt601 = bt709;

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
inline constexpr float kY_R = 0.183f, kY_G = 0.614f, kY_B = 0.062f;
inline constexpr float kCb_R = -0.101f, kCb_G = -0.339f, kCb_B = 0.439f;
inline constexpr float kCr_R = 0.439f, kCr_G = -0.399f, kCr_B = -0.040f;
inline constexpr std::uint8_t kYMin = 16, kYMax = 235;
inline constexpr std::uint8_t kCMin = 16, kCMax = 240;
}
namespace bt601_rgb2yuv = bt709_rgb2yuv;

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

struct MatrixCoeffs {
    float r_cr, g_cb, g_cr, b_cb;
};
inline constexpr MatrixCoeffs kMatFullBT601{1.402f, -0.344f, -0.714f, 1.772f};
inline constexpr MatrixCoeffs kMatLtdBT601{1.596f, -0.392f, -0.813f, 2.017f};
inline constexpr MatrixCoeffs kMatFullBT709{1.5748f, -0.1873f, -0.4681f, 1.8556f};
inline constexpr MatrixCoeffs kMatLtdBT709{1.793f, -0.213f, -0.533f, 2.112f};
inline constexpr MatrixCoeffs kMatFullBT2020{1.4746f, -0.1645f, -0.5714f, 1.8814f};
inline constexpr MatrixCoeffs kMatLtdBT2020{1.679f, -0.187f, -0.650f, 2.142f};
inline constexpr float kLimitedLumaScale = 1.164f;

inline MatrixCoeffs matrix_coeffs(const ColorMatrix m, const ColorRange r) {
    const bool ltd = r == ColorRange::Limited;
    switch (m) {
        case ColorMatrix::BT601: return ltd ? kMatLtdBT601 : kMatFullBT601;
        case ColorMatrix::BT2020: return ltd ? kMatLtdBT2020 : kMatFullBT2020;
        default: return ltd ? kMatLtdBT709 : kMatFullBT709;
    }
}

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

}