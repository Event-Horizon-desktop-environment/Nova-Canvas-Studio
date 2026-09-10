// CUDA-accelerated RGBA -> NV12 conversion + resize.
//
// Compiled with nvcc (CUDA 12 & 13). The kernel is intentionally a single
// __global__ that does bilinear resize + BT.709 limited-range RGB->YUV in one
// launch and writes straight into the planes of an FFmpeg AV_PIX_FMT_CUDA hw
// frame, so the normal encode loop never touches CPU for the color conversion.

#include "canvas/core/gpu/cuda_convert.hpp"
#include "canvas/core/util/log.hpp"

#include <cuda_runtime.h>

#include <cstdio>

namespace canvas::core::gpu {

using cuda_event_t = cudaEvent_t;

namespace {

// Bilinear-resize + RGBA -> NV12. yPlane holds Y (dst_h * dst_w), uvPlane holds
// interleaved CbCr at (dst_h/2) * dst_w. BT.709 limited range (matches the
// source's actual tagged color space — see exporter.cpp's BT.709 stream tags;
// this was previously BT.601, which mismatched the output and produced the
// same magenta/purple skin-tone shift on playback of exported files).
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

    // CbCr at 2x2 subsampling (x,y both even). Interleaved: Cb, Cr.
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

// Normalize a fractional coordinate to a valid clamp index [0, n-2] and a
// fraction in [0,1], matching the RGBA kernel's edge handling above.
__device__ inline int clamp_index(float v, int n) {
    if (v < 0.f) return 0;
    if (v >= n - 1.f) return n - 2;
    return (int)v;
}

// Bilinear-resize a GPU NV12 source into a letterboxed rectangle on a GPU NV12
// target, writing the whole target plane (content + bars). The bars are black
// (Y=16, Cb=Cr=128) so every pixel of the output hw frame stays defined.
// Output is `ow x oh`; the scaled content occupies (dx,dy)..(dx+dstW,dy+dstH).
// The output is normally the full encoder hw frame (letterbox already applied).
// `fade` in (0,1] dips the CONTENT toward black in-place (16 + (Y-16)*fade,
// 128 + (C-128)*fade) — the whole-canvas edge-fade blend the CPU compositor
// applies for a single clip's transition; 1.0 leaves pixels untouched.
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

    // Chroma plane is subsampled by 2 in each axis; interleaved Cb,Cr.
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

}  // namespace

bool cuda_available() {
    static const bool ok = [] {
        int dev = -1;
        return cudaGetDevice(&dev) == cudaSuccess && dev >= 0;
    }();
    return ok;
}

namespace {
// Persistent non-blocking stream for the GPU composite path. Kernels launch
// here asynchronously so the CPU never stalls on a full device sync, letting
// NVDEC (decode) and NVENC (encode) run concurrently with the composite.
cudaStream_t& convert_stream() {
    static cudaStream_t s = [] {
        cudaStream_t st = nullptr;
        if (cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking) != cudaSuccess)
            st = nullptr;
        return st;
    }();
    return s;
}

// CUDA error-transition logger: prints exactly one "[gpu] cuda error" line at
// the FIRST failure of a site after a run of successes, then stays quiet while
// it keeps failing, and prints one "[gpu] cuda recovered" line when the site
// returns to success. A driver reset / context loss reads as a single one-shot
// transition instead of a per-frame spam, and the recovery line marks the end
// of the outage for log-timestamp alignment with the FFmpeg switches above.
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
}  // namespace

bool convert_nv12_resize_async(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                               std::size_t src_y_pitch, std::size_t src_uv_pitch,
                               uint8_t* dY, std::size_t yPitch,
                               uint8_t* dUV, std::size_t uvPitch,
                               int out_w, int out_h, int dst_w, int dst_h,
                               int dx, int dy, float fade) {
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
    return gpu_cuda_check("nv12_resize_async", cudaGetLastError());
}

bool convert_nv12_sync() {
    cudaStream_t s = convert_stream();
    if (!s) return false;
    cudaStreamSynchronize(s);
    return cudaGetLastError() == cudaSuccess;
}

// Full-device barrier.  Unlike convert_nv12_sync (which only waits on our own
// resize stream), this waits for every kernel launched on the CUDA context —
// including the asynchronous NVENC encode that is reading the encoder input
// surface.  Call it BEFORE returning an encoder hw-frame to the surface pool
// (av_frame_free) so the producer can never recycle a surface NVENC is still
// reading (which produced duplicate/repeating frames).
bool convert_nv12_device_sync() {
    const cudaError_t e = cudaDeviceSynchronize();
    cudaGetLastError();
    return gpu_cuda_check("nv12_device_sync", e);
}

// How many times the resize-event ring found a full slot (consumer a full ring
// behind). File-scope so convert_nv12_record_event can bump it and
// nv12_pool_stalls() can consume-on-read it across calls; TU-local by design.
static uint64_t s_stalls = 0;

bool convert_nv12_record_event(void** out) {
    if (!out) return false;
    cudaStream_t s = convert_stream();
    if (!s) return false;
    // Recycle pre-created events: cudaEventCreate per call costs ~tens of
    // microseconds and would dominate the composite launch at high throughput.
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
        // Still busy: the consumer is behind by a full ring — encode is the
        // slower side. Fall back to a blocking record so ordering is preserved.
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
    // Check the event regardless of whether the stream is still valid; if the
    // stream is gone the event may still exist (created elsewhere).
    const cudaError_t e = cudaEventSynchronize(reinterpret_cast<cuda_event_t>(ev));
    (void)s;
    return e == cudaSuccess;
}

void convert_nv12_destroy_event(void* ev) {
    // Events come from the internal ring pool; recycling is handled by
    // convert_nv12_record_event. Nothing to free.
    (void)ev;
}

uint64_t nv12_pool_stalls() {
    // Consume-on-read counter: how many times convert_nv12_record_event found
    // the 64-slot event ring fully busy (the resize consumer is a full ring
    // behind). A nonzero delta on the `[render]` line is the signature of an
    // encode-bound export.
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

    // Always-on (once): the CUDA RGBA->NV12 kernel hardcodes BT.709 limited
    // RGB->YUV coefficients + clamps (Y[16,235]/C[16,240]) — see rgbaToNV12.
    // The export/hw path CONVERTS with BT.709 here, while the CPU swscale path
    // converts with its BT.601 default: two different matrices for the same
    // export depending on hw vs cpu. Traced against the [export] color audit.
    static bool rgba_nv12_logged_ = false;
    if (!rgba_nv12_logged_) {
        rgba_nv12_logged_ = true;
        ::canvas::core::log::log_warning(
            "[gpu] rgbaToNV12 kernel: matrix=bt709 range=limited (hardcoded "
            "coeffs, clamps Y[16,235] C[16,240]) — hw/export path only");
    }

    // Persistent buffer avoids per-frame cudaMalloc/cudaFree (~14.7MB at 1440p).
    // The buffer is lazily allocated and only reallocated if the source grows.
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

bool convert_nv12_resize_to_host(const uint8_t* srcY, const uint8_t* srcUV,
                                 int src_w, int src_h,
                                 std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                 int out_w, int out_h, int dst_w, int dst_h,
                                 int dx, int dy,
                                 std::vector<uint8_t>* outY, std::vector<uint8_t>* outUV) {
    if (!srcY || !srcUV || !outY || !outUV || src_w <= 0 || src_h <= 0 || out_w <= 0 ||
        out_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    uint8_t* dY = nullptr;
    uint8_t* dUV = nullptr;
    const std::size_t y_sz = static_cast<std::size_t>(out_w) * out_h;
    const std::size_t uv_sz = static_cast<std::size_t>(out_w) * (out_h / 2);
    if (cudaMalloc(&dY, y_sz) != cudaSuccess) return false;
    if (cudaMalloc(&dUV, uv_sz) != cudaSuccess) {
        cudaFree(dY);
        return false;
    }

    bool ok = false;
    if (convert_nv12_resize(srcY, srcUV, src_w, src_h, src_y_pitch, src_uv_pitch,
                            dY, static_cast<std::size_t>(out_w),
                            dUV, static_cast<std::size_t>(out_w),
                            out_w, out_h, dst_w, dst_h, dx, dy)) {
        outY->resize(y_sz);
        outUV->resize(uv_sz);
        const cudaError_t ey = cudaMemcpy(outY->data(), dY, y_sz, cudaMemcpyDeviceToHost);
        const cudaError_t euv = cudaMemcpy(outUV->data(), dUV, uv_sz, cudaMemcpyDeviceToHost);
        ok = (ey == cudaSuccess) && (euv == cudaSuccess);
    }
    cudaFree(dUV);
    cudaFree(dY);
    return ok;
}

}  // namespace canvas::core::gpu
