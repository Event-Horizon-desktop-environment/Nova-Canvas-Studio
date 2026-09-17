#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/util/log.hpp"

#include <cuda_runtime.h>

#include <cstdio>
#include <mutex>

namespace canvas::core::gpu {

using cuda_event_t = cudaEvent_t;

namespace {

__global__ void rgbaToNV12(const uint8_t* __restrict__ src, int sw, int sh,
                           uint8_t* __restrict__ yPlane, size_t yPitch,
                           uint8_t* __restrict__ uvPlane, size_t uvPitch,
                           int dw, int dh) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh) return;

    const float sx = ((float)x + 0.5f) * sw / dw - 0.5f;
    const float sy = ((float)y + 0.5f) * sh / dh - 0.5f;
    int ix = sx < 0 ? 0 : (sx >= sw - 1 ? sw - 2 : (int)sx);
    int iy = sy < 0 ? 0 : (sy >= sh - 1 ? sh - 2 : (int)sy);
    float fx = sx - ix, fy = sy - iy;
    if (fx < 0) fx = 0; else if (fx > 1) fx = 1;
    if (fy < 0) fy = 0; else if (fy > 1) fy = 1;

    const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
    const float w01 = (1 - fx) * fy,     w11 = fx * fy;
    const uint8_t* p00 = src + ((size_t)iy * sw + ix) * 4;
    const uint8_t* p10 = p00 + 4;
    const uint8_t* p01 = p00 + (size_t)sw * 4;
    const uint8_t* p11 = p01 + 4;
    const float r = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
    const float g = w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
    const float b = w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];

    float Y = 16.f + (0.183f * r + 0.614f * g + 0.062f * b);
    if (Y < 16.f) Y = 16.f; else if (Y > 235.f) Y = 235.f;
    yPlane[(size_t)y * yPitch + x] = (uint8_t)(Y + 0.5f);

    if ((x & 1) == 0 && (y & 1) == 0) {
        const int cx = x >> 1, cy = y >> 1;
        float Cb = 128.f + (-0.101f * r - 0.339f * g + 0.439f * b);
        float Cr = 128.f + (0.439f * r - 0.399f * g - 0.040f * b);
        if (Cb < 16.f) Cb = 16.f; else if (Cb > 240.f) Cb = 240.f;
        if (Cr < 16.f) Cr = 16.f; else if (Cr > 240.f) Cr = 240.f;
        uint8_t* uv = uvPlane + ((size_t)cy * uvPitch + (size_t)cx * 2);
        uv[0] = (uint8_t)(Cb + 0.5f);
        uv[1] = (uint8_t)(Cr + 0.5f);
    }
}

__device__ inline int clamp_index(float v, int n) {
    if (v < 0.f) return 0;
    if (v >= n - 1.f) return n - 2;
    return (int)v;
}

__global__ void nv12Resize(const uint8_t* __restrict__ srcY,
                           const uint8_t* __restrict__ srcUV,
                           int sw, int sh, size_t sYPitch, size_t sUVPitch,
                           int dstW, int dstH, int dx, int dy,
                           uint8_t* __restrict__ outY, size_t oYPitch,
                           uint8_t* __restrict__ outUV, size_t oUVPitch,
                           int ow, int oh, float fade) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= ow || y >= oh) return;

    const int rdx = x - dx, rdy = y - dy;
    if (rdx >= 0 && rdx < dstW && rdy >= 0 && rdy < dstH) {
        const float sx = ((float)rdx + 0.5f) * sw / dstW - 0.5f;
        const float sy = ((float)rdy + 0.5f) * sh / dstH - 0.5f;
        int ix = clamp_index(sx, sw), iy = clamp_index(sy, sh);
        float fx = sx - ix, fy = sy - iy;
        if (fx < 0) fx = 0; else if (fx > 1) fx = 1;
        if (fy < 0) fy = 0; else if (fy > 1) fy = 1;
        const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
        const float w01 = (1 - fx) * fy,     w11 = fx * fy;
        const uint8_t* p00 = srcY + (size_t)iy * sYPitch + ix;
        const uint8_t* p10 = p00 + 1;
        const uint8_t* p01 = p00 + sYPitch;
        const uint8_t* p11 = p01 + 1;
        float Y = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
        outY[(size_t)y * oYPitch + x] = (uint8_t)(16.f + (Y - 16.f) * fade + 0.5f);
    } else {
        outY[(size_t)y * oYPitch + x] = 16;
    }

    if ((x & 1) == 0 && (y & 1) == 0) {
        const int cx = x >> 1, cy = y >> 1;
        const int srccw = sw >> 1, srcch = sh >> 1;
        const int cdw = dstW >> 1, cdh = dstH >> 1;
        const int cdx = dx >> 1, cdy = dy >> 1;
        const int crdx = cx - cdx, crdy = cy - cdy;
        uint8_t* dst = outUV + (size_t)cy * oUVPitch + (size_t)cx * 2;
        if (crdx >= 0 && crdx < cdw && crdy >= 0 && crdy < cdh) {
            const float scx = ((float)crdx + 0.5f) * srccw / cdw - 0.5f;
            const float scy = ((float)crdy + 0.5f) * srcch / cdh - 0.5f;
            int icx = clamp_index(scx, srccw), icy = clamp_index(scy, srcch);
            float fcx = scx - icx, fcy = scy - icy;
            if (fcx < 0) fcx = 0; else if (fcx > 1) fcx = 1;
            if (fcy < 0) fcy = 0; else if (fcy > 1) fcy = 1;
            const float w00 = (1 - fcx) * (1 - fcy), w10 = fcx * (1 - fcy);
            const float w01 = (1 - fcx) * fcy,     w11 = fcx * fcy;
            const uint8_t* p00 = srcUV + (size_t)icy * sUVPitch + (size_t)icx * 2;
            const uint8_t* p01 = p00 + sUVPitch;
            const float cb =
                w00 * p00[0] + w10 * (p00[2]) + w01 * p01[0] + w11 * (p01[2]);
            const float cr =
                w00 * p00[1] + w10 * (p00[3]) + w01 * p01[1] + w11 * (p01[3]);
            dst[0] = (uint8_t)(128.f + (cb - 128.f) * fade + 0.5f);
            dst[1] = (uint8_t)(128.f + (cr - 128.f) * fade + 0.5f);
        } else {
            dst[0] = 128;
            dst[1] = 128;
        }
    }
}

__global__ void nv12GradeResize(const uint8_t* __restrict__ srcY,
                                const uint8_t* __restrict__ srcUV,
                                int sw, int sh, size_t sYPitch, size_t sUVPitch,
                                int dstW, int dstH, int dx, int dy,
                                uint8_t* __restrict__ outY, size_t oYPitch,
                                uint8_t* __restrict__ outUV, size_t oUVPitch,
                                int ow, int oh, float fade,
                                GradeKernelParams g) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= ow || y >= oh) return;

    const int rdx = x - dx, rdy = y - dy;
    const bool in_rect = rdx >= 0 && rdx < dstW && rdy >= 0 && rdy < dstH;

    const int bx = x >> 1, by = y >> 1;
    float Y = 16.f, Cb = 128.f, Cr = 128.f;
    if (in_rect) {
        const float sx = ((float)rdx + 0.5f) * sw / dstW - 0.5f;
        const float sy = ((float)rdy + 0.5f) * sh / dstH - 0.5f;
        int ix = clamp_index(sx, sw), iy = clamp_index(sy, sh);
        float fx = sx - ix, fy = sy - iy;
        if (fx < 0) fx = 0; else if (fx > 1) fx = 1;
        if (fy < 0) fy = 0; else if (fy > 1) fy = 1;
        const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
        const float w01 = (1 - fx) * fy,     w11 = fx * fy;
        const uint8_t* p00 = srcY + (size_t)iy * sYPitch + ix;
        const uint8_t* p10 = p00 + 1;
        const uint8_t* p01 = p00 + sYPitch;
        const uint8_t* p11 = p01 + 1;
        Y = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];

        const int srccw = sw >> 1, srcch = sh >> 1;
        const int cdw = dstW >> 1, cdh = dstH >> 1;
        const int cdx = dx >> 1, cdy = dy >> 1;
        const int crdx = bx - cdx, crdy = by - cdy;
        if (crdx >= 0 && crdx < cdw && crdy >= 0 && crdy < cdh) {
            const float scx = ((float)crdx + 0.5f) * srccw / cdw - 0.5f;
            const float scy = ((float)crdy + 0.5f) * srcch / cdh - 0.5f;
            int icx = clamp_index(scx, srccw), icy = clamp_index(scy, srcch);
            float fcx = scx - icx, fcy = scy - icy;
            if (fcx < 0) fcx = 0; else if (fcx > 1) fcx = 1;
            if (fcy < 0) fcy = 0; else if (fcy > 1) fcy = 1;
            const float w00 = (1 - fcx) * (1 - fcy), w10 = fcx * (1 - fcy);
            const float w01 = (1 - fcx) * fcy,     w11 = fcx * fcy;
            const uint8_t* q00 = srcUV + (size_t)icy * sUVPitch + (size_t)icx * 2;
            const uint8_t* q01 = q00 + sUVPitch;
            Cb = w00 * q00[0] + w10 * (q00[2]) + w01 * q01[0] + w11 * (q01[2]);
            Cr = w00 * q00[1] + w10 * (q00[3]) + w01 * q01[1] + w11 * (q01[3]);
        }
    }

    const float yr = (g.range == 1) ? Y : 1.164f * (Y - 16.f);
    const float Cbq = Cb - 128.f, Crq = Cr - 128.f;
    float r = yr + g.r_cr * Crq;
    float g_ = yr + g.g_cb * Cbq + g.g_cr * Crq;
    float b = yr + g.b_cb * Cbq;
    if (r < 0.f) r = 0.f; else if (r > 255.f) r = 255.f;
    if (g_ < 0.f) g_ = 0.f; else if (g_ > 255.f) g_ = 255.f;
    if (b < 0.f) b = 0.f; else if (b > 255.f) b = 255.f;
    float rn = r * (1.f / 255.f), gn = g_ * (1.f / 255.f), bn = b * (1.f / 255.f);

    if (g.lut && g.lut_size >= 2) {
        const int last = g.lut_size - 1;
        if (rn < 0.f) rn = 0.f; else if (rn > 1.f) rn = 1.f;
        if (gn < 0.f) gn = 0.f; else if (gn > 1.f) gn = 1.f;
        if (bn < 0.f) bn = 0.f; else if (bn > 1.f) bn = 1.f;
        const float ur = rn * (float)last;
        const float vg = gn * (float)last;
        const float wb = bn * (float)last;
        int r0 = (int)ur, g0 = (int)vg, b0 = (int)wb;
        if (r0 < 0) r0 = 0; else if (r0 > last) r0 = last;
        if (g0 < 0) g0 = 0; else if (g0 > last) g0 = last;
        if (b0 < 0) b0 = 0; else if (b0 > last) b0 = last;
        const int r1 = r0 < last ? r0 + 1 : r0;
        const int g1 = g0 < last ? g0 + 1 : g0;
        const int b1 = b0 < last ? b0 + 1 : b0;
        const float fr = ur - (float)r0, fg = vg - (float)g0, fb = wb - (float)b0;
        const size_t nsz = (size_t)g.lut_size;
        const size_t i000 = ((size_t)r0 * nsz + (size_t)g0) * nsz + (size_t)b0;
        const size_t i100 = ((size_t)r1 * nsz + (size_t)g0) * nsz + (size_t)b0;
        const size_t i001 = ((size_t)r0 * nsz + (size_t)g0) * nsz + (size_t)b1;
        const size_t i101 = ((size_t)r1 * nsz + (size_t)g0) * nsz + (size_t)b1;
        const size_t i010 = ((size_t)r0 * nsz + (size_t)g1) * nsz + (size_t)b0;
        const size_t i110 = ((size_t)r1 * nsz + (size_t)g1) * nsz + (size_t)b0;
        const size_t i011 = ((size_t)r0 * nsz + (size_t)g1) * nsz + (size_t)b1;
        const size_t i111 = ((size_t)r1 * nsz + (size_t)g1) * nsz + (size_t)b1;
        const float* d = g.lut;
        for (int c = 0; c < 3; ++c) {
            const float c00 = d[i000 * 3u + (size_t)c] +
                              (d[i001 * 3u + (size_t)c] - d[i000 * 3u + (size_t)c]) * fb;
            const float c10 = d[i100 * 3u + (size_t)c] +
                              (d[i101 * 3u + (size_t)c] - d[i100 * 3u + (size_t)c]) * fb;
            const float c01 = d[i010 * 3u + (size_t)c] +
                              (d[i011 * 3u + (size_t)c] - d[i010 * 3u + (size_t)c]) * fb;
            const float c11 = d[i110 * 3u + (size_t)c] +
                              (d[i111 * 3u + (size_t)c] - d[i110 * 3u + (size_t)c]) * fb;
            const float c0 = c00 + (c01 - c00) * fg;
            const float c1 = c10 + (c11 - c10) * fg;
            const float v = c0 + (c1 - c0) * fr;
            if (c == 0) r = v * 255.f;
            else if (c == 1) g_ = v * 255.f;
            else b = v * 255.f;
        }
    }

    if (fade < 1.f) {
        r *= fade;
        g_ *= fade;
        b *= fade;
    }

    float Yo = 16.f + (0.183f * r + 0.614f * g_ + 0.062f * b);
    if (Yo < 16.f) Yo = 16.f; else if (Yo > 235.f) Yo = 235.f;
    outY[(size_t)y * oYPitch + x] = (uint8_t)(Yo + 0.5f);

    if ((x & 1) == 0 && (y & 1) == 0) {
        float Cbo = 128.f + (-0.101f * r - 0.339f * g_ + 0.439f * b);
        float Cro = 128.f + (0.439f * r - 0.399f * g_ - 0.040f * b);
        if (Cbo < 16.f) Cbo = 16.f; else if (Cbo > 240.f) Cbo = 240.f;
        if (Cro < 16.f) Cro = 16.f; else if (Cro > 240.f) Cro = 240.f;
        uint8_t* uv = outUV + (size_t)by * oUVPitch + (size_t)bx * 2;
        uv[0] = (uint8_t)(Cbo + 0.5f);
        uv[1] = (uint8_t)(Cro + 0.5f);
    }
}

__global__ void nv12TitleBlend(const uint8_t* __restrict__ sp, int spw, int sph,
                               int sox, int soy,
                               uint8_t* __restrict__ outY, size_t oYPitch,
                               uint8_t* __restrict__ outUV, size_t oUVPitch,
                               int ow, int oh, float fade) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= ow || y >= oh) return;

    const int sx = x - sox, sy = y - soy;
    const bool hit = (unsigned)sx < (unsigned)spw && (unsigned)sy < (unsigned)sph;
    float A = 0.f, pR = 0.f, pG = 0.f, pB = 0.f;
    if (hit) {
        const uint8_t* pp = sp + ((size_t)sy * spw + sx) * 4;
        pR = pp[0];
        pG = pp[1];
        pB = pp[2];
        A = pp[3] * (1.f / 255.f);
    }
    if (A > 0.f) {
        const float Yv = outY[(size_t)y * oYPitch + x];
        const float Ys = 16.f + (0.183f * pR + 0.614f * pG + 0.062f * pB);
        float Yo = 16.f + fade * (Ys - 16.f) + (1.f - A) * (Yv - 16.f);
        if (Yo < 16.f) Yo = 16.f; else if (Yo > 235.f) Yo = 235.f;
        outY[(size_t)y * oYPitch + x] = (uint8_t)(Yo + 0.5f);
    }

    if ((x & 1) == 0 && (y & 1) == 0) {
        float A00 = A;
        float rq = pR, gq = pG, bq = pB;
        if (!hit || A00 == 0.f) {
            float asum = 0.f, rsum = 0.f, gsum = 0.f, bsum = 0.f;
#pragma unroll
            for (int dyy = 0; dyy < 2; ++dyy) {
                for (int dxx = 0; dxx < 2; ++dxx) {
                    const int px = x + dxx - sox, py = y + dyy - soy;
                    if ((unsigned)px < (unsigned)spw && (unsigned)py < (unsigned)sph) {
                        const uint8_t* pp = sp + ((size_t)py * spw + px) * 4;
                        rsum += pp[0];
                        gsum += pp[1];
                        bsum += pp[2];
                        asum += pp[3];
                    }
                }
            }
            A00 = asum * (0.25f / 255.f);
            rq = rsum * 0.25f;
            gq = gsum * 0.25f;
            bq = bsum * 0.25f;
        }
        if (A00 > 0.f) {
            const int bx = x >> 1, by = y >> 1;
            uint8_t* uv = outUV + (size_t)by * oUVPitch + (size_t)bx * 2;
            const float Cbv = uv[0], Crv = uv[1];
            const float Cbs = 128.f + (-0.101f * rq - 0.339f * gq + 0.439f * bq);
            const float Crs = 128.f + (0.439f * rq - 0.399f * gq - 0.040f * bq);
            float Cbo = 128.f + fade * (Cbs - 128.f) + (1.f - A00) * (Cbv - 128.f);
            float Cro = 128.f + fade * (Crs - 128.f) + (1.f - A00) * (Crv - 128.f);
            if (Cbo < 16.f) Cbo = 16.f; else if (Cbo > 240.f) Cbo = 240.f;
            if (Cro < 16.f) Cro = 16.f; else if (Cro > 240.f) Cro = 240.f;
            uv[0] = (uint8_t)(Cbo + 0.5f);
            uv[1] = (uint8_t)(Cro + 0.5f);
        }
    }
}

}

bool cuda_available() {
    static const bool ok = [] {
        int dev = -1;
        return cudaGetDevice(&dev) == cudaSuccess && dev >= 0;
    }();
    return ok;
}

namespace {
cudaStream_t& convert_stream() {
    static cudaStream_t s = [] {
        cudaStream_t st = nullptr;
        if (cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking) != cudaSuccess)
            st = nullptr;
        return st;
    }();
    return s;
}

bool gpu_cuda_check(const char* name, const cudaError_t e) {
    static thread_local const char* s_bad = nullptr;
    if (e == cudaSuccess) {
        if (s_bad) {
            fprintf(stderr, "[gpu] cuda recovered name=%s\n", s_bad);
            s_bad = nullptr;
        }
        return true;
    }
    if (s_bad != name) {
        s_bad = name;
        fprintf(stderr, "[gpu] cuda error name=%s err=%s\n", name, cudaGetErrorName(e));
    }
    return false;
}

void launch_title_blend(const TitleSpriteGpu* title, uint8_t* dY, std::size_t yPitch,
                        uint8_t* dUV, std::size_t uvPitch, int ow, int oh, float fade,
                        cudaStream_t s) {
    if (!title || !title->valid() || ow <= 0 || oh <= 0) return;
    const int x0 = title->ox - 1;
    const int y0 = title->oy - 1;
    const int x1 = title->ox + title->w;
    const int y1 = title->oy + title->h;
    const int lx0 = x0 < 0 ? 0 : x0;
    const int ly0 = y0 < 0 ? 0 : y0;
    const int lx1 = x1 >= ow ? ow - 1 : x1;
    const int ly1 = y1 >= oh ? oh - 1 : y1;
    if (lx1 < lx0 || ly1 < ly0) return;
    const dim3 blk(16, 16);
    const dim3 grp((lx1 - lx0 + 1 + 15) / 16, (ly1 - ly0 + 1 + 15) / 16);
    nv12TitleBlend<<<grp, blk, 0, s>>>(title->rgba, title->w, title->h, title->ox, title->oy,
                                       dY, yPitch, dUV, uvPitch, ow, oh, fade);
}
}

bool convert_nv12_resize_async(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                               std::size_t src_y_pitch, std::size_t src_uv_pitch,
                               uint8_t* dY, std::size_t yPitch,
                               uint8_t* dUV, std::size_t uvPitch,
                               int out_w, int out_h, int dst_w, int dst_h,
                               int dx, int dy, float fade, const TitleSpriteGpu* title) {
    if (!srcY || !srcUV || !dY || !dUV || src_w <= 0 || src_h <= 0 || out_w <= 0 ||
        out_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;
    cudaStream_t s = convert_stream();
    if (!s) return false;

    cudaGetLastError();
    const dim3 blk(16, 16);
    const dim3 grp((out_w + 15) / 16, (out_h + 15) / 16);
    nv12Resize<<<grp, blk, 0, s>>>(srcY, srcUV, src_w, src_h, src_y_pitch, src_uv_pitch,
                                   dst_w, dst_h, dx, dy, dY, yPitch, dUV, uvPitch,
                                   out_w, out_h, fade);
    launch_title_blend(title, dY, yPitch, dUV, uvPitch, out_w, out_h, fade, s);
    return gpu_cuda_check("nv12_resize_async", cudaGetLastError());
}

bool convert_nv12_sync() {
    cudaStream_t s = convert_stream();
    if (!s) return false;
    cudaStreamSynchronize(s);
    return cudaGetLastError() == cudaSuccess;
}

bool convert_nv12_device_sync() {
    const cudaError_t e = cudaDeviceSynchronize();
    cudaGetLastError();
    return gpu_cuda_check("nv12_device_sync", e);
}

static uint64_t s_stalls = 0;

bool convert_nv12_record_event(void** out) {
    if (!out) return false;
    cudaStream_t s = convert_stream();
    if (!s) return false;
    static constexpr int kPoolSize = 64;
    static cuda_event_t s_events[kPoolSize];
    static bool s_created[kPoolSize] = {false};
    static int s_head = 0;
    cuda_event_t& ev = s_events[s_head % kPoolSize];
    const int slot = s_head % kPoolSize;
    if (!s_created[slot]) {
        if (!gpu_cuda_check("nv12_event_create",
                            cudaEventCreateWithFlags(&ev, cudaEventDisableTiming)))
            return false;
        s_created[slot] = true;
    } else if (cudaEventQuery(ev) != cudaSuccess) {
        ++s_stalls;
        cudaEventSynchronize(ev);
    }
    ++s_head;
    cudaGetLastError();
    if (!gpu_cuda_check("nv12_event_record", cudaEventRecord(ev, s))) return false;
    *out = ev;
    return true;
}

bool convert_nv12_wait_event(void* ev) {
    if (!ev) return false;
    cudaStream_t s = convert_stream();
    const cudaError_t e = cudaEventSynchronize(reinterpret_cast<cuda_event_t>(ev));
    (void)s;
    return e == cudaSuccess;
}

void convert_nv12_destroy_event(void* ev) {
    (void)ev;
}

uint64_t nv12_pool_stalls() {
    static uint64_t s_stalls_seen = 0;
    const uint64_t now = s_stalls;
    const uint64_t delta = now - s_stalls_seen;
    s_stalls_seen = now;
    return delta;
}

bool convert_rgba_to_nv12(const uint8_t* rgba, int src_w, int src_h,
                          uint8_t* dY, size_t yPitch,
                          uint8_t* dUV, size_t uvPitch,
                          int dst_w, int dst_h) {
    if (!rgba || !dY || !dUV || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    static bool rgba_nv12_logged_ = false;
    if (!rgba_nv12_logged_) {
        rgba_nv12_logged_ = true;
        ::canvas::core::log::log_warning(
            "[gpu] rgbaToNV12 kernel: matrix=bt709 range=limited (hardcoded "
            "coeffs, clamps Y[16,235] C[16,240]) — hw/export path only");
    }

    thread_local uint8_t* dRgba = nullptr;
    thread_local size_t dRgbaSize = 0;
    const size_t needed = (size_t)src_w * src_h * 4;
    if (!dRgba || dRgbaSize < needed) {
        if (dRgba) cudaFree(dRgba);
        dRgba = nullptr;
        dRgbaSize = 0;
        if (cudaMalloc(&dRgba, needed) != cudaSuccess) return false;
        dRgbaSize = needed;
    }
    const bool uploaded =
        cudaMemcpy(dRgba, rgba, needed, cudaMemcpyHostToDevice) ==
        cudaSuccess;

    bool ok = false;
    if (uploaded) {
        const dim3 blk(16, 16);
        const dim3 grp((dst_w + 15) / 16, (dst_h + 15) / 16);
        rgbaToNV12<<<grp, blk>>>(dRgba, src_w, src_h, dY, yPitch, dUV, uvPitch,
                                dst_w, dst_h);
        cudaDeviceSynchronize();
        ok = cudaGetLastError() == cudaSuccess;
    }
    return ok;
}

bool convert_nv12_resize(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                         std::size_t src_y_pitch, std::size_t src_uv_pitch,
                         uint8_t* dY, std::size_t yPitch,
                         uint8_t* dUV, std::size_t uvPitch,
                         int out_w, int out_h, int dst_w, int dst_h,
                         int dx, int dy, float fade) {
    if (!srcY || !srcUV || !dY || !dUV || src_w <= 0 || src_h <= 0 || out_w <= 0 ||
        out_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    if (cudaGetLastError() != cudaSuccess)
        cudaGetLastError();

    const dim3 blk(16, 16);
    const dim3 grp((out_w + 15) / 16, (out_h + 15) / 16);
    nv12Resize<<<grp, blk>>>(srcY, srcUV, src_w, src_h, src_y_pitch, src_uv_pitch,
                             dst_w, dst_h, dx, dy, dY, yPitch, dUV, uvPitch,
                             out_w, out_h, fade);
    cudaDeviceSynchronize();
    return cudaGetLastError() == cudaSuccess;
}

namespace {

std::mutex g_staging_mtx;
uint8_t* g_staging_y = nullptr;
uint8_t* g_staging_uv = nullptr;
std::size_t g_staging_y_size = 0;
std::size_t g_staging_uv_size = 0;
}

bool convert_nv12_resize_to_host(const uint8_t* srcY, const uint8_t* srcUV,
                                 int src_w, int src_h,
                                 std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                 int out_w, int out_h, int dst_w, int dst_h,
                                 int dx, int dy,
                                 std::vector<uint8_t>* outY, std::vector<uint8_t>* outUV) {
    if (!srcY || !srcUV || !outY || !outUV || src_w <= 0 || src_h <= 0 || out_w <= 0 ||
        out_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    const std::size_t y_sz = static_cast<std::size_t>(out_w) * out_h;
    const std::size_t uv_sz = static_cast<std::size_t>(out_w) * (out_h / 2);
    std::lock_guard<std::mutex> lk(g_staging_mtx);
    if (y_sz > g_staging_y_size) {
        if (g_staging_y) cudaFree(g_staging_y);
        g_staging_y = nullptr;
        if (cudaMalloc(&g_staging_y, y_sz) != cudaSuccess) return false;
        g_staging_y_size = y_sz;
    }
    if (uv_sz > g_staging_uv_size) {
        if (g_staging_uv) cudaFree(g_staging_uv);
        g_staging_uv = nullptr;
        if (cudaMalloc(&g_staging_uv, uv_sz) != cudaSuccess) return false;
        g_staging_uv_size = uv_sz;
    }

    bool ok = false;
    if (convert_nv12_resize(srcY, srcUV, src_w, src_h, src_y_pitch, src_uv_pitch,
                            g_staging_y, static_cast<std::size_t>(out_w),
                            g_staging_uv, static_cast<std::size_t>(out_w),
                            out_w, out_h, dst_w, dst_h, dx, dy)) {
        outY->resize(y_sz);
        outUV->resize(uv_sz);
        const cudaError_t ey = cudaMemcpy(outY->data(), g_staging_y, y_sz, cudaMemcpyDeviceToHost);
        const cudaError_t euv = cudaMemcpy(outUV->data(), g_staging_uv, uv_sz, cudaMemcpyDeviceToHost);
        ok = (ey == cudaSuccess) && (euv == cudaSuccess);
    }
    return ok;
}

const char* cuda_last_error_string() {
    return cudaGetErrorString(cudaGetLastError());
}

void* grade_lut_upload(const float* data, int size) {
    if (!data || size < 2) return nullptr;
    const uint64_t bytes = (uint64_t)size * size * size * 3u * sizeof(float);
    if (bytes == 0 || bytes > (uint64_t)1u << 32) return nullptr;
    void* d = nullptr;
    if (cudaMalloc(&d, bytes) != cudaSuccess) return nullptr;
    if (cudaMemcpy(d, data, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
        cudaFree(d);
        return nullptr;
    }
    return d;
}

void grade_lut_free(void* dev) {
    if (dev) cudaFree(dev);
}

bool title_sprite_upload(const uint8_t* rgba, int w, int h, int ox, int oy,
                         TitleSpriteGpu* out) {
    if (!rgba || !out || w <= 0 || h <= 0) return false;
    const uint64_t bytes = (uint64_t)w * h * 4u;
    void* d = nullptr;
    if (cudaMalloc(&d, bytes) != cudaSuccess) return false;
    if (cudaMemcpy(d, rgba, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
        cudaFree(d);
        return false;
    }
    out->rgba = static_cast<const uint8_t*>(d);
    out->w = w;
    out->h = h;
    out->ox = ox;
    out->oy = oy;
    return true;
}

void title_sprite_free(TitleSpriteGpu* spr) {
    if (!spr) return;
    if (spr->rgba) cudaFree(const_cast<uint8_t*>(spr->rgba));
    *spr = TitleSpriteGpu{};
}

bool convert_nv12_grade_resize_async(const uint8_t* srcY, const uint8_t* srcUV,
                                     int src_w, int src_h,
                                     std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                     uint8_t* dY, std::size_t yPitch,
                                     uint8_t* dUV, std::size_t uvPitch,
                                     int out_w, int out_h, int dst_w, int dst_h,
                                     int dx, int dy, float fade,
                                     const GradeKernelParams& g,
                                     const TitleSpriteGpu* title) {
    if (!srcY || !srcUV || !dY || !dUV || src_w <= 0 || src_h <= 0 || out_w <= 0 ||
        out_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;
    if (!g.lut || g.lut_size < 2) return false;
    cudaStream_t s = convert_stream();
    if (!s) return false;

    cudaGetLastError();
    const dim3 blk(16, 16);
    const dim3 grp((out_w + 15) / 16, (out_h + 15) / 16);
    nv12GradeResize<<<grp, blk, 0, s>>>(srcY, srcUV, src_w, src_h, src_y_pitch,
                                        src_uv_pitch, dst_w, dst_h, dx, dy,
                                        dY, yPitch, dUV, uvPitch,
                                        out_w, out_h, fade, g);
    launch_title_blend(title, dY, yPitch, dUV, uvPitch, out_w, out_h, fade, s);
    return gpu_cuda_check("nv12_grade_resize_async", cudaGetLastError());
}

}