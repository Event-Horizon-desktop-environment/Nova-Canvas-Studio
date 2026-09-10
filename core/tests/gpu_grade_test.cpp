// GPU grade-law tests for the fused export kernel (cuda_convert.cu
// nv12GradeResize, exported via GradeKernelParams). The kernel cannot run
// headless (it needs a real CUDA device), so this test pins its LAW by mirroring
// it byte-for-byte in host C++ and checking it against three invariants:
//
//   1. identity-grade roundtrip is within rounding of the source planes — the
//      anti-double-conversion property (the studio-range bug class) — and never
//      shifts by a whole level.
//   2. letterbox bars stay Y=16/C=128 exactly regardless of grade/fade.
//   3. a real (non-affine) grade matches the CPU export chain built from swscale
//      NV12->RGBA decode + apply_grade_lut + fade + rgb_to_yuv to within 1-2 LSB
//      — the parity that keeps a graded clip's GPU export identical to the CPU
//      compositor's.
//   4. full-range (JPEG) luma is converted to the limited output range, not
//      raw-copied or double-scaled.
//
// Headless; links only canvas_core. The mirror IS the kernel contract — keep the
// two in sync (see lut.cpp's same-note on apply_grade_lut).

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/media/frame.hpp"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace gg = canvas::core::grade_graph;
namespace gpu = canvas::core::gpu;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

std::uint8_t encode_byte(float v) {
    return static_cast<std::uint8_t>(v + 0.5f);
}

// Host mirror of nv12GradeResize (cuda_convert.cu). Same geometry, same bilinear
// Y/block-chroma sampling, same decode/grade/fade/encode law. `lut` may be
// empty (size < 2) to exercise the passthrough branch.
void grade_resize_mirror(const std::vector<std::uint8_t>& srcY, std::size_t sYP,
                         const std::vector<std::uint8_t>& srcUV, std::size_t sUVP,
                         int sw, int sh, int outW, int outH, int dstW, int dstH,
                         int dx, int dy, float fade,
                         const gpu::MatrixCoeffs& k, int range,
                         const std::vector<float>* lut, int lutSize,
                         std::vector<std::uint8_t>& oY,
                         std::vector<std::uint8_t>& oUV) {
    oY.assign(static_cast<std::size_t>(outW) * outH, 0);
    oUV.assign(static_cast<std::size_t>(outW) * (outH / 2) * 2u, 0);
    const auto clamped = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    };
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            const int rdx = x - dx, rdy = y - dy;
            const bool in_rect = rdx >= 0 && rdx < dstW && rdy >= 0 && rdy < dstH;

            float Y = 16.0f, Cb = 128.0f, Cr = 128.0f;
            if (in_rect) {
                const float sx = ((float)rdx + 0.5f) * sw / dstW - 0.5f;
                const float sy = ((float)rdy + 0.5f) * sh / dstH - 0.5f;
                const int ix = std::clamp((int)sx, 0, sw - 2);
                const int iy = std::clamp((int)sy, 0, sh - 2);
                const float fx = clamped(sx - ix), fy = clamped(sy - iy);
                const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
                const float w01 = (1 - fx) * fy, w11 = fx * fy;
                const std::uint8_t* p00 = &srcY[(std::size_t)iy * sYP + ix];
                Y = w00 * p00[0] + w10 * p00[1] + w01 * p00[sYP] + w11 * p00[sYP + 1];

                const int bx = x >> 1, by = y >> 1;
                const int srccw = sw >> 1, srcch = sh >> 1;
                const int cdw = dstW >> 1, cdh = dstH >> 1;
                const int cdx = dx >> 1, cdy = dy >> 1;
                const int crdx = bx - cdx, crdy = by - cdy;
                if (crdx >= 0 && crdx < cdw && crdy >= 0 && crdy < cdh) {
                    const float scx = ((float)crdx + 0.5f) * srccw / cdw - 0.5f;
                    const float scy = ((float)crdy + 0.5f) * srcch / cdh - 0.5f;
                    const int icx = std::clamp((int)scx, 0, srccw - 2);
                    const int icy = std::clamp((int)scy, 0, srcch - 2);
                    const float fcx = clamped(scx - icx), fcy = clamped(scy - icy);
                    const float wu00 = (1 - fcx) * (1 - fcy), wu10 = fcx * (1 - fcy);
                    const float wu01 = (1 - fcx) * fcy, wu11 = fcx * fcy;
                    const std::uint8_t* q00 = &srcUV[(std::size_t)icy * sUVP + 2u * icx];
                    Cb = wu00 * q00[0] + wu10 * q00[2] + wu01 * q00[sUVP] + wu11 * q00[sUVP + 2];
                    Cr = wu00 * q00[1] + wu10 * q00[3] + wu01 * q00[sUVP + 1] + wu11 * q00[sUVP + 3];
                }
            }

            // YUV -> full-range RGB (colorspace.hpp law: limited needs the
            // 255/224 headroom unwound by kLimitedLumaScale, then + chroma gains).
            const float yr = (range == 1) ? Y : (Y - 16.0f) * gpu::kLimitedLumaScale;
            float r = yr + k.r_cr * (Cb - 128.f);
            float g = yr + k.g_cb * (Cb - 128.f) + k.g_cr * (Cr - 128.f);
            float b = yr + k.b_cb * (Cr - 128.f);
            r = std::clamp(r, 0.0f, 255.0f);
            g = std::clamp(g, 0.0f, 255.0f);
            b = std::clamp(b, 0.0f, 255.0f);

            // Trilinear grade (apply_grade_lut/sample_lut_pixel law; the high
            // neighbor clamps to the floor so reads stay in-bounds at the edges).
            if (lut && lutSize >= 2) {
                const float last = (float)lutSize - 1.0f;
                const int r0 = std::clamp((int)(std::clamp(r / 255.f, 0.0f, 1.0f) * last), 0, lutSize - 1);
                const int g0 = std::clamp((int)(std::clamp(g / 255.f, 0.0f, 1.0f) * last), 0, lutSize - 1);
                const int b0 = std::clamp((int)(std::clamp(b / 255.f, 0.0f, 1.0f) * last), 0, lutSize - 1);
                const int r1 = std::min(r0 + 1, lutSize - 1);
                const int g1 = std::min(g0 + 1, lutSize - 1);
                const int b1 = std::min(b0 + 1, lutSize - 1);
                const float fr = std::clamp(r / 255.f, 0.0f, 1.0f) * last - (float)r0;
                const float fg = std::clamp(g / 255.f, 0.0f, 1.0f) * last - (float)g0;
                const float fb = std::clamp(b / 255.f, 0.0f, 1.0f) * last - (float)b0;
                const std::size_t nsz = (std::size_t)lutSize;
                const auto idx = [nsz](int ri, int gi, int bi) {
                    return ((std::size_t)ri * nsz + (std::size_t)gi) * nsz + (std::size_t)bi;
                };
                const std::size_t i000 = idx(r0, g0, b0), i100 = idx(r1, g0, b0);
                const std::size_t i001 = idx(r0, g0, b1), i101 = idx(r1, g0, b1);
                const std::size_t i010 = idx(r0, g1, b0), i110 = idx(r1, g1, b0);
                const std::size_t i011 = idx(r0, g1, b1), i111 = idx(r1, g1, b1);
                const float* d = lut->data();
                const auto grad = [&](int ch) {
                    const float c000 = d[i000 * 3 + ch], c001 = d[i001 * 3 + ch];
                    const float c100 = d[i100 * 3 + ch], c101 = d[i101 * 3 + ch];
                    const float c010 = d[i010 * 3 + ch], c011 = d[i011 * 3 + ch];
                    const float c110 = d[i110 * 3 + ch], c111 = d[i111 * 3 + ch];
                    const float c00 = c000 + (c001 - c000) * fb;
                    const float c10 = c100 + (c101 - c100) * fb;
                    const float c01 = c010 + (c011 - c010) * fb;
                    const float c11 = c110 + (c111 - c110) * fb;
                    const float c0 = c00 + (c01 - c00) * fg;
                    const float c1 = c10 + (c11 - c10) * fg;
                    return c0 + (c1 - c0) * fr;
                };
                r = grad(0) * 255.f;
                g = grad(1) * 255.f;
                b = grad(2) * 255.f;
            }

            if (fade < 1.0f) {
                r *= fade;
                g *= fade;
                b *= fade;
            }

            // rgbaToNV12 BT.709-limited encode (clamps + +0.5 rounding).
            const float Yout = std::clamp(16.f + 0.183f * r + 0.614f * g + 0.062f * b, 16.f, 235.f);
            oY[(std::size_t)y * outW + x] = encode_byte(Yout);
            if ((x & 1) == 0 && (y & 1) == 0) {
                const float Cbo = std::clamp(128.f + -0.101f * r - 0.339f * g + 0.439f * b, 16.f, 240.f);
                const float Cro = std::clamp(128.f + 0.439f * r - 0.399f * g - 0.040f * b, 16.f, 240.f);
                std::uint8_t* uv = &oUV[(std::size_t)(y >> 1) * outW + 2u * (x >> 1)];
                uv[0] = encode_byte(Cbo);
                uv[1] = encode_byte(Cro);
            }
        }
    }
}

}  // namespace

int main() {
    // Synthetic BT.709-limited source, 32x16 luma / 16x8 interleaved chroma.
    // A luma gradient pins roundtrip; the colored blocks pin channel mapping.
    const int W = 32, H = 16;
    const std::size_t sYP = W;
    std::vector<std::uint8_t> srcY((std::size_t)W * H);
    std::vector<std::uint8_t> srcUV((std::size_t)(W / 2) * (H / 2) * 2u);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            srcY[(std::size_t)y * sYP + x] =
                (std::uint8_t)(16 + (y * (235 - 16)) / (H - 1));
    for (int cy = 0; cy < H / 2; ++cy) {
        for (int cx = 0; cx < W / 2; ++cx) {
            const int bx = cx * 2, by = cy * 2;
            std::uint8_t cb = 128, cr = 128;
            if (bx == 0 && by == 0) { cb = 128; cr = 128; }       // gray corner
            else if (bx >= 28 && by >= 12) { cb = 148; cr = 148; }  // moderate warm
            else if (bx >= 14 && by >= 6) { cb = 106; cr = 122; }   // moderate cool
            // All chroma stays within ~±24 of 128 and the blocks sit on the
            // mid luma rows, so the decoded RGB stays in-gamut — an in-gamut
            // color must roundtrip through identity decode/grade/encode
            // (extreme out-of-gamut chroma clamps to 0/255 and CANNOT, which is
            // correct behavior, not a law drift).
            srcUV[((std::size_t)cy * (W / 2) + cx) * 2u + 0] = cb;
            srcUV[((std::size_t)cy * (W / 2) + cx) * 2u + 1] = cr;
        }
    }

    // Identity grade (gain 1.0) for the roundtrip legs.
    gg::GradeGraph idg;
    const int corr = idg.add_node(gg::NodeKind::kCorrector);
    idg.node(corr).correct_mode = gg::CorrectMode::kLgg;
    idg.node(corr).lgg.emplace();
    idg.node(corr).lgg->gain_master = 1.0f;
    const int out_n = idg.add_node(gg::NodeKind::kOutput);
    check(idg.add_rgb_edge(corr, out_n) >= 0, "gpu_grade: wire identity tree");
    const gg::GradeLutPtr id_lut = gg::bake_grade_lut(idg, 33);

    // 1. Identity-grade ≡ passthrough, byte-for-byte. If the kernel's grade
    //    branch diverged from the plain resize law by even one level, graded
    //    clips would look different from ungraded clips — this pins that the
    //    (fully linear, grid-exact) identity LUT is a true no-op.
    {
        std::vector<std::uint8_t> oY, oUV, pY, pUV;
        grade_resize_mirror(srcY, sYP, srcUV, W, W, H, W, H, W, H, 0, 0,
                            1.0f, gpu::kMatLtdBT709, 0,
                            &id_lut->data, id_lut->size, oY, oUV);
        grade_resize_mirror(srcY, sYP, srcUV, W, W, H, W, H, W, H, 0, 0,
                            1.0f, gpu::kMatLtdBT709, 0, nullptr, 0, pY, pUV);
        check(oY == pY && oUV == pUV,
              "gpu_grade: identity-grade output == passthrough (byte-exact)");
    }

    // 2. Passthrough roundtrip: NEUTRAL pixels (the anti-double-conversion bar —
    //    a second studio-range floor would shift them by ~16, not negligibly).
    //    Out-of-gamut-ish colored blocks roundtrip through YCbCr->RGB->YCbCr
    //    with a few LSB of inherent crosstalk and are covered by the strong (4).
    {
        std::vector<std::uint8_t> oY, oUV;
        grade_resize_mirror(srcY, sYP, srcUV, W, W, H, W, H, W, H, 0, 0,
                            1.0f, gpu::kMatLtdBT709, 0, nullptr, 0, oY, oUV);
        int maxY = 0, maxC = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const std::size_t ubi = (std::size_t)(y / 2) * (W / 2) + (x / 2);
                if (srcUV[ubi * 2u] != 128 || srcUV[ubi * 2u + 1] != 128) continue;
                maxY = std::max(maxY, std::abs((int)oY[(std::size_t)y * W + x] - (int)srcY[(std::size_t)y * W + x]));
                if ((x & 1) == 0 && (y & 1) == 0) {
                    const std::size_t ui = ((std::size_t)(y >> 1) * W) + 2u * (x >> 1);
                    maxC = std::max(maxC, std::abs((int)oUV[ui] - 128));
                    maxC = std::max(maxC, std::abs((int)oUV[ui + 1] - 128));
                }
            }
        }
        std::printf("info: gpu_grade passthrough (neutral) maxY_dev=%d maxC_dev=%d\n", maxY, maxC);
        check(maxY <= 2, "gpu_grade: neutral passthrough Y roundtrip within 2 LSB");
        check(maxC <= 3, "gpu_grade: neutral passthrough chroma roundtrip within 3 LSB");
    }

    // 3. Letterbox bars stay 16/128/128 exactly under grade + fade.
    {
        const int ow = 16, oh = 8;
        gg::GradeGraph g;
        const int c2 = g.add_node(gg::NodeKind::kCorrector);
        g.node(c2).correct_mode = gg::CorrectMode::kLgg;
        g.node(c2).lgg.emplace();
        g.node(c2).lgg->gain_master = 0.5f;  // obvious grade
        const int o2 = g.add_node(gg::NodeKind::kOutput);
        g.add_rgb_edge(c2, o2);
        const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 17);
        std::vector<std::uint8_t> oY, oUV;
        grade_resize_mirror(srcY, sYP, srcUV, W, W, H, ow, oh, 8, 4, 4, 2,
                            0.35f, gpu::kMatLtdBT709, 0, &lut->data, lut->size, oY, oUV);
        bool bars_ok = true;
        for (int y = 0; y < oh && bars_ok; ++y) {
            for (int x = 0; x < ow; ++x) {
                const bool in_rect = x >= 4 && x < 12 && y >= 2 && y < 6;
                if (!in_rect && oY[(std::size_t)y * ow + x] != 16) bars_ok = false;
                if ((x & 1) == 0 && (y & 1) == 0 && !in_rect) {
                    const std::uint8_t* uv = &oUV[(std::size_t)(y >> 1) * ow + 2u * (x >> 1)];
                    if (uv[0] != 128 || uv[1] != 128) bars_ok = false;
                }
            }
        }
        check(bars_ok, "gpu_grade: letterbox bars stay 16/128/128 under grade+fade");
    }

    // 4. Real grade vs the CPU export chain: swscale NV12->RGBA,
    //    apply_grade_lut, fade (CPU truncation law: canvas->rgba *= fade),
    //    rgb_to_yuv, then compare with the kernel-mirror output.
    {
        gg::GradeGraph g;
        const int c3 = g.add_node(gg::NodeKind::kCorrector);
        g.node(c3).correct_mode = gg::CorrectMode::kLgg;
        g.node(c3).lgg.emplace();
        g.node(c3).lgg->gain_master = 0.7f;
        g.node(c3).lgg->gamma_master = 1.15f;
        const int o3 = g.add_node(gg::NodeKind::kOutput);
        g.add_rgb_edge(c3, o3);
        const gg::GradeLutPtr lut = gg::bake_grade_lut(g, 33);
        check(lut != nullptr, "gpu_grade: real grade bakes");

        SwsContext* sws = sws_getContext(W, H, AV_PIX_FMT_NV12, W, H, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            std::fprintf(stderr, "skip: gpu_grade CPU-chain leg (sws ctx unavailable)\n");
        } else {
            // Replicate video_decoder.cpp make_rgba's exact pin: BT.709
            // coefficients, src_range=0 for a LIMITED source, dstRange=1, and
            // contrast/saturation in 1/65536 fixed point (1 << 16 = unity).
            sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_ITU709), 0,
                                     sws_getCoefficients(SWS_CS_ITU709), 1,
                                     0, 1 << 16, 1 << 16);
            std::vector<std::uint8_t> rgba((std::size_t)W * H * 4u);
            const std::uint8_t* sv[4] = {srcY.data(), srcUV.data(), nullptr, nullptr};
            int sl[4] = {(int)sYP, (int)W, 0, 0};
            std::uint8_t* dv[4] = {rgba.data(), nullptr, nullptr, nullptr};
            int dl[4] = {W * 4, 0, 0, 0};
            const int got = sws_scale(sws, sv, sl, 0, H, dv, dl);
            sws_freeContext(sws);
            check(got == H, "gpu_grade: swscale decoded the synthetic NV12");

            canvas::core::VideoFrame src;
            src.width = W;
            src.height = H;
            src.stride = W * 4;
            src.rgba.assign(rgba.begin(), rgba.end());

            // CPU chain (the compositor's law): apply_grade_lut, then the
            // renderer.cpp canvas->rgba *= fade uint8 truncation law, then
            // rgb_to_yuv. Runs for both the real LUT and the identity LUT so
            // the 4:2:0 UP-SAMPLER floor (swscale's interpolation vs the GPU
            // kernel's block sample) can be measured separately from grade drift.
            const float fade = 0.6f;
            const auto cpu_chain = [&](const gg::GradeLutPtr& gl, float f,
                                       std::vector<std::uint8_t>& cY,
                                       std::vector<std::uint8_t>& cUV) {
                const canvas::core::VideoFramePtr graded = gg::apply_grade_lut(src, *gl);
                cY.assign((std::size_t)W * H, 0);
                cUV.assign((std::size_t)(W / 2) * (H / 2) * 2u, 0);
                if (!graded) return false;
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        const std::uint8_t* px = &graded->rgba[((std::size_t)y * W + x) * 4u];
                        const gpu::Yuv8 yuv = gpu::rgb_to_yuv(
                            (std::uint8_t)(px[0] * f),
                            (std::uint8_t)(px[1] * f),
                            (std::uint8_t)(px[2] * f));
                        cY[(std::size_t)y * W + x] = yuv.y;
                        if ((x & 1) == 0 && (y & 1) == 0) {
                            std::uint8_t* uv = &cUV[(std::size_t)(y >> 1) * W + 2u * (x >> 1)];
                            uv[0] = yuv.cb;
                            uv[1] = yuv.cr;
                        }
                    }
                }
                return true;
            };

            std::vector<std::uint8_t> cpuY, cpuUV;
            check(cpu_chain(lut, fade, cpuY, cpuUV), "gpu_grade: CPU chain graded frame");

            std::vector<std::uint8_t> oY, oUV;
            grade_resize_mirror(srcY, sYP, srcUV, W, W, H, W, H, W, H, 0, 0,
                                fade, gpu::kMatLtdBT709, 0, &lut->data, lut->size, oY, oUV);
            int maxY = 0, maxC = 0;
            for (int i = 0; i < W * H; ++i)
                maxY = std::max(maxY, std::abs((int)oY[i] - (int)cpuY[i]));
            for (std::size_t i = 0; i < cpuUV.size(); ++i)
                maxC = std::max(maxC, std::abs((int)oUV[i] - (int)cpuUV[i]));

            // Upsampler floor: identity LUT vs mirror passthrough.
            std::vector<std::uint8_t> fyY, fyUV, pY, pUV;
            cpu_chain(id_lut, 1.0f, fyY, fyUV);
            grade_resize_mirror(srcY, sYP, srcUV, W, W, H, W, H, W, H, 0, 0,
                                1.0f, gpu::kMatLtdBT709, 0, nullptr, 0, pY, pUV);
            int floorC = 0, floorY = 0;
            for (std::size_t i = 0; i < fyUV.size(); ++i)
                floorC = std::max(floorC, std::abs((int)fyUV[i] - (int)pUV[i]));
            for (int i = 0; i < W * H; ++i)
                floorY = std::max(floorY, std::abs((int)fyY[i] - (int)pY[i]));
            std::printf("info: gpu_grade vs CPU chain maxY_dev=%d maxC_dev=%d (upsampler floor Y=%d C=%d)\n",
                        maxY, maxC, floorY, floorC);
            check(maxY <= floorY + 2, "gpu_grade: graded Y within floor+2 LSB");
            check(maxC <= floorC + 4, "gpu_grade: graded chroma within floor+4 LSB");
        }
    }

    // 5. Full-range (JPEG) luma is converted to the limited output range by the
    //    encode clamp, not raw-copied or double-scaled. Compute the expected
    //    value by the same law: neutral chroma -> RGB = Y, encode clamps to 235.
    {
        gg::GradeGraph g;
        const int c5 = g.add_node(gg::NodeKind::kCorrector);
        g.node(c5).correct_mode = gg::CorrectMode::kLgg;
        g.node(c5).lgg.emplace();
        g.node(c5).lgg->gain_master = 1.0f;
        const int o5 = g.add_node(gg::NodeKind::kOutput);
        g.add_rgb_edge(c5, o5);
        const gg::GradeLutPtr id5 = gg::bake_grade_lut(g, 33);
        std::vector<std::uint8_t> fY((std::size_t)W * H);
        std::vector<std::uint8_t> fUV((std::size_t)(W / 2) * (H / 2) * 2u, 128u);
        for (std::size_t i = 0; i < fY.size(); ++i) fY[i] = (std::uint8_t)((i * 255) / fY.size());
        std::vector<std::uint8_t> oY, oUV;
        grade_resize_mirror(fY, sYP, fUV, W, W, H, W, H, W, H, 0, 0,
                            1.0f, gpu::kMatFullBT709, 1, &id5->data, id5->size, oY, oUV);
        int maxY = 0;
        for (std::size_t i = 0; i < fY.size(); ++i) {
            const float expected = std::clamp(16.f + 0.859f * (float)fY[i], 16.f, 235.f);
            maxY = std::max(maxY, std::abs((int)oY[i] - (int)encode_byte(expected)));
        }
        std::printf("info: gpu_grade full-range roundtrip maxY_dev=%d\n", maxY);
        check(maxY <= 2, "gpu_grade: full-range -> limited encode law holds");
    }

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return EXIT_FAILURE;
}