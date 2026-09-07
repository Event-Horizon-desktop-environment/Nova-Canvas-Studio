#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace canvas::core::gpu {

// Returns whether the CUDA runtime is usable on this machine (initialized once,
// cheaply). Always safe to call; used to choose the GPU encode path.
bool cuda_available();

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
                                int, int, int, int) { return false; }
inline bool convert_nv12_resize_async(const uint8_t*, const uint8_t*, int, int, std::size_t,
                                      std::size_t, uint8_t*, std::size_t, uint8_t*, std::size_t,
                                      int, int, int, int, int, int) { return false; }
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
bool convert_nv12_resize(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                         std::size_t src_y_pitch, std::size_t src_uv_pitch,
                         uint8_t* dY, std::size_t yPitch,
                         uint8_t* dUV, std::size_t uvPitch,
                         int out_w, int out_h, int dst_w, int dst_h,
                         int dx, int dy);

// Asynchronous variant of convert_nv12_resize.
bool convert_nv12_resize_async(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                               std::size_t src_y_pitch, std::size_t src_uv_pitch,
                               uint8_t* dY, std::size_t yPitch,
                               uint8_t* dUV, std::size_t uvPitch,
                               int out_w, int out_h, int dst_w, int dst_h,
                               int dx, int dy);
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
#endif  // CANVAS_HAVE_CUDA

}  // namespace canvas::core::gpu
