#pragma once

// Color space conversion helpers shared by the preview viewer (GPU NV12 upload +
// CPU NV12->RGBA fallback) and the export/NVENC path. Keeping the coefficients and
// the pixel math in ONE place means the two paths can never drift (they previously
// duplicated the BT.601 limited-range constants).
//
// BT.601 limited range: Y in [16,235], Cb/Cr in [16,240] centered on 128.
// RGB out is per-pixel, clamped to [0,255].
//
// This header is deliberately Qt-free so it is usable from both the GUI viewer and
// the headless export engine. It is also included by cuda_convert.cu.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace canvas::core::gpu {

namespace bt601 {
// YUV -> RGB (limited range) coefficients and offsets. The viewer's CPU fallback and
// the CUDA kernel must agree on these exactly. Stored as floats matching the kernel.
inline constexpr float kYOffset = 16.0f;   // black-level Y
inline constexpr float kCbCrCenter = 128.0f;
inline constexpr float kRY = 1.164f;
inline constexpr float kRCr = 1.596f;
inline constexpr float kGY = 1.164f;
inline constexpr float kGCb = -0.392f;
inline constexpr float kGCr = -0.813f;
inline constexpr float kBY = 1.164f;
inline constexpr float kBCb = 2.017f;
}  // namespace bt601

// Converts a single BT.601 limited-range pixel. All inputs are the raw 8-bit YUV
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

namespace bt601_rgb2yuv {
// RGB -> YUV (BT.601 limited range) coefficients, the inverse of the yuv_to_rgb
// path above. This is the reference for the CUDA kernel (cuda_convert.cu), which
// implements the same math on-device; keeping the constants here is the single
// source of truth for the export color path. Y in [16,235], Cb/Cr in [16,240].
inline constexpr float kY_R = 0.257f, kY_G = 0.504f, kY_B = 0.098f;
inline constexpr float kCb_R = -0.148f, kCb_G = -0.291f, kCb_B = 0.439f;
inline constexpr float kCr_R = 0.439f, kCr_G = -0.368f, kCr_B = -0.071f;
inline constexpr std::uint8_t kYMin = 16, kYMax = 235;
inline constexpr std::uint8_t kCMin = 16, kCMax = 240;
}  // namespace bt601_rgb2yuv

// RGB -> YUV reference (BT.601 limited range). CPU reference for the CUDA path;
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

}  // namespace canvas::core::gpu
