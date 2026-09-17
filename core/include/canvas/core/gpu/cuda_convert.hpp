#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace canvas::core::gpu {

bool cuda_available();

struct GradeKernelParams {
    const float* lut = nullptr;
    int lut_size = 0;
    float r_cr = 1.793f, g_cb = -0.213f, g_cr = -0.533f, b_cb = 2.112f;
    int range = 0;
};

struct TitleSpriteGpu {
    const uint8_t* rgba = nullptr;
    int w = 0;
    int h = 0;
    int ox = 0;
    int oy = 0;
    [[nodiscard]] bool valid() const noexcept { return rgba != nullptr && w > 0 && h > 0; }
};

#ifndef CANVAS_HAVE_CUDA
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
bool convert_rgba_to_nv12(const uint8_t* rgba, int src_w, int src_h,
                          uint8_t* dY, std::size_t yPitch,
                          uint8_t* dUV, std::size_t uvPitch,
                          int dst_w, int dst_h);

bool convert_nv12_resize(const uint8_t* srcY, const uint8_t* srcUV, int src_w, int src_h,
                         std::size_t src_y_pitch, std::size_t src_uv_pitch,
                         uint8_t* dY, std::size_t yPitch,
                         uint8_t* dUV, std::size_t uvPitch,
                         int out_w, int out_h, int dst_w, int dst_h,
                         int dx, int dy, float fade = 1.0f);

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
uint64_t nv12_pool_stalls();
bool convert_nv12_sync();
bool convert_nv12_device_sync();

bool convert_nv12_resize_to_host(const uint8_t* srcY, const uint8_t* srcUV,
                                 int src_w, int src_h,
                                 std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                 int out_w, int out_h, int dst_w, int dst_h,
                                 int dx, int dy,
                                 std::vector<uint8_t>* outY, std::vector<uint8_t>* outUV);

const char* cuda_last_error_string();

void* grade_lut_upload(const float* data, int size);
void grade_lut_free(void* dev);

bool title_sprite_upload(const uint8_t* rgba, int w, int h, int ox, int oy, TitleSpriteGpu* out);
void title_sprite_free(TitleSpriteGpu* spr);

bool convert_nv12_grade_resize_async(const uint8_t* srcY, const uint8_t* srcUV,
                                     int src_w, int src_h,
                                     std::size_t src_y_pitch, std::size_t src_uv_pitch,
                                     uint8_t* dY, std::size_t yPitch,
                                     uint8_t* dUV, std::size_t uvPitch,
                                     int out_w, int out_h, int dst_w, int dst_h,
                                     int dx, int dy, float fade = 1.0f,
                                     const GradeKernelParams& g = {},
                                     const TitleSpriteGpu* title = nullptr);
#endif

}
