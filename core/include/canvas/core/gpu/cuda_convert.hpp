#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace canvas::core::gpu {

// Returns whether the CUDA runtime is usable on this machine (initialized once,
// cheaply). Always safe to call; used to choose the GPU encode path.
bool cuda_available();

// Fixed-GPU params for the fused grade/resize kernel (convert_nv12_grade_resize_async).
// `lut` must be a DEVICE-resident copy of a baked grade grid produced by
// grade_lut_upload() (r-major layout data[((r*N)+g)*N + b], N = lut_size); the
// four chroma gains are the (matrix, range) chroma coefficients selected by the
// caller from colorspace.hpp's matrix_coeffs() so the law is never re-derived
// here, and `range` (0 = limited, 1 = full) selects the 1.164 limited-luma
// unwinding. Defaults are the (BT709, Limited) pair for a zero-initialized struct.
struct GradeKernelParams {
    const float* lut = nullptr;
    int lut_size = 0;
    float r_cr = 1.793f, g_cb = -0.213f, g_cr = -0.533f, b_cb = 2.112f;
    int range = 0;
};

// Device-resident premultiplied-RGBA8 title-overlay sprite (the GPU upload of
// title::raster_title_sprite), with its canvas origin. `rgba` is DEVICE memory
// owned by the caller until title_sprite_free; `valid()` gates an overlay
// launch.
struct TitleSpriteGpu {
    const uint8_t* rgba = nullptr;
    int w = 0;
    int h = 0;
    int ox = 0;  // canvas x of the sprite's left edge
    int oy = 0;  // canvas y of the sprite's top edge
    [[nodiscard]] bool valid() const noexcept { return rgba != nullptr && w > 0 && h > 0; }
};

#ifndef CANVAS_HAVE_CUDA
// When the GPU path is not compiled in (no nvcc / CANVAS_HAVE_CUDA undefined), these
// declare compile-time fallbacks so callers can keep calling them unconditionally;
// they simply report "not available" and the CPU code path is used instead. This
// keeps canvas_core linkable with or without CUDA.
inline bool cuda_available() { return false; }
inline bool convert_rgba_to_nv12(const uint8_t*, int, int, uint8_t*, std::size_t, uint8_t*,
                                 std::size_t, int, int) { return false; }
inline bool convert_nv12_resize(const uint8_t*, const uint8_t*, int, int, std::size_t,
                                std::size_t, uint8_t*, std::size_t, uint8_t*, std::size_t, int, int,
                                int, int, int, int, float = 1.0f) { return false; }
inline bool convert_nv12_resize_async(const uint8_t*, const uint8_t*, int, int, std::size_t,
                                      std::size_t, uint8_t*, std::size_t, uint8_t*, std::size_t,
                                      int, int, int, int, int, int, float = 1.0f,
                                      const TitleSpriteGpu* = nullptr) { return false; }
inline bool convert_nv12_record_event(void**) { return false; }
inline bool convert_nv12_wait_event(void*) { return true; }
inline void convert_nv12_destroy_event(void*) {}
inline uint64_t nv12_pool_stalls() { return 0; }
inline bool convert_nv12_sync() { return true; }
inline bool convert_nv12_device_sync() { return true; }
inline bool convert_nv12_resize_to_host(const uint8_t*, const uint8_t*, int, int, std::size_t,
                                        std::size_t, int, int, int, int, int, int,
                                        std::vector<uint8_t>*, std::vector<uint8_t>*) {
    return false;
}
inline const char* cuda_last_error_string() { return "no-cuda"; }
inline void* grade_lut_upload(const float*, int) { return nullptr; }
inline void grade_lut_free(void*) {}
inline bool convert_nv12_grade_resize_async(const uint8_t*, const uint8_t*, int, int,
                                            std::size_t, std::size_t, uint8_t*, std::size_t,
                                            uint8_t*, std::size_t, int, int, int, int,
                                            int, int, float = 1.0f,
                                            const GradeKernelParams& = {},
                                            const TitleSpriteGpu* = nullptr) {
    return false;
}
inline bool title_sprite_upload(const uint8_t*, int, int, int, int, TitleSpriteGpu*) {
    return false;
}
inline void title_sprite_free(TitleSpriteGpu*) {}
#else
// Converts a single host RGBA frame to NV12 directly on the GPU, resizing to
// dst_w x dst_h with bilinear filtering. The output NV12 is written into
// caller-provided CUDA device buffers (exactly the planes of an FFmpeg
// AV_PIX_FMT_CUDA hw frame) so NVENC can be fed without a CPU round-trip.
bool convert_rgba_to_nv12(const uint8_t* rgba, int src_w, int src_h,
                          uint8_t* dY, std::size_t yPitch,
                          uint8_t* dUV, std::size_t uvPitch,
                          int dst_w, int dst_h);

// Bilinear-resizes an existing GPU NV12 frame into a letterboxed rectangle on
// a GPU NV12 target, writing every target pixel (content and black bars).
// `fade` in (0,1] dips the content toward black (whole-canvas edge-fade blend,
// matching the CPU compositor's per-clip transition factor); 1.0 is identity.
bool convert_nv12_resize(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                         std::size_t src_y_pitch, std::size_t src_uv_pitch,
                         uint8_t* dY, std::size_t yPitch,
                         uint8_t* dUV, std::size_t uvPitch,
                         int out_w, int out_h, int dst_w, int dst_h,
                         int dx, int dy, float fade = 1.0f);

// Asynchronous variant of convert_nv12_resize.
bool convert_nv12_resize_async(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                               std::size_t src_y_pitch, std::size_t src_uv_pitch,
                               uint8_t* dY, std::size_t yPitch,
                               uint8_t* dUV, std::size_t uvPitch,
                               int out_w, int out_h, int dst_w, int dst_h,
                               int dx, int dy, float fade = 1.0f,
                               const TitleSpriteGpu* title = nullptr);
bool convert_nv12_record_event(void** out);
bool convert_nv12_wait_event(void* ev);
void convert_nv12_destroy_event(void* ev);
// Consume-on-read counter of convert_nv12_record_event ring-busy stalls.
uint64_t nv12_pool_stalls();
bool convert_nv12_sync();
// Full-device barrier: waits for NVENC's async read of an encoder input surface
// so the surface can be safely returned to the pool before reuse.
bool convert_nv12_device_sync();

// GPU-composite a hardware-decoded NV12 source into a host readable NV12 frame.
bool convert_nv12_resize_to_host(const uint8_t* srcY, const uint8_t* srcUV,
                                 int src_w, int src_h,
                                 std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                 int out_w, int out_h, int dst_w, int dst_h,
                                 int dx, int dy,
                                 std::vector<uint8_t>* outY, std::vector<uint8_t>* outUV);

// Human-readable description of the last CUDA runtime error (consumes the sticky
// error like cudaGetLastError), or a stable fallback when CUDA isn't compiled in.
const char* cuda_last_error_string();

// Uploads a baked grade grid (GradeLut3D::data) into device memory for the
// grade kernel. Returns a device pointer (caller frees with grade_lut_free()),
// or nullptr on any failure. ~432KB at the default 33^3 bake — global memory
// (L2/L1-cached), not __constant__, because __constant__ is capped at 64KB.
// Do not free while a kernel that may still be reading it is queued.
void* grade_lut_upload(const float* data, int size);
void grade_lut_free(void* dev);

// Uploads a host premultiplied-RGBA8 title sprite (title::TitleSprite::data
// from raster_title_sprite, with its canvas origin) into device memory and
// fills `out`. False on any failure (out left untouched). Free with
// title_sprite_free; do not free while a queued kernel may still read it.
bool title_sprite_upload(const uint8_t* rgba, int w, int h, int ox, int oy, TitleSpriteGpu* out);
void title_sprite_free(TitleSpriteGpu* spr);

// Fused NV12 resize + grade for graded clips on the export fast path — the
// graded successor to convert_nv12_resize(_async). Same letterbox geometry and
// bilinear law as nv12Resize, then YUV->RGB (GradeKernelParams gains), the 3D
// LUT grade (explicit r-major index — no GL R/B axis swap), the whole-canvas
// edge-fade RGB dip, and the exact rgbaToNV12 BT.709-limited encode. Replaces
// convert_nv12_resize_async when a clip is graded so graded exports stay on the
// NVENC path instead of dropping to the CPU RGBA blit. The optional `title`
// sprite (see TitleSpriteGpu / raster_title_sprite) is fused over the faded
// result on the same launch — its premultiplied coverage over the
// already-faded canvas matches the CPU compositor's title-over-faded-video
// order.
bool convert_nv12_grade_resize_async(const uint8_t* srcY, const uint8_t* srcUV,
                                     int src_w, int src_h,
                                     std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                     uint8_t* dY, std::size_t yPitch,
                                     uint8_t* dUV, std::size_t uvPitch,
                                     int out_w, int out_h, int dst_w, int dst_h,
                                     int dx, int dy, float fade = 1.0f,
                                     const GradeKernelParams& g = {},
                                     const TitleSpriteGpu* title = nullptr);
#endif  // CANVAS_HAVE_CUDA

}  // namespace canvas::core::gpu
