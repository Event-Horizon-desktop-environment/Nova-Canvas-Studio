#pragma once

// VAAPI surface export → core-surface translation.
//
// Turns the raw `VADRMPRIMESurfaceDescriptor` that vaExportSurfaceHandle()
// fills in into an owning VaapiSurface (dup'd fds + per-plane geometry + a pin
// that keeps the source VA surface reserved). Pure data mapping — no libva
// calls here — so it is unit-testable headlessly by fabricating a descriptor
// (vaapi_surface_test), while the decode worker (video_decoder.cpp) owns the
// actual libva call against the driver.

#include "canvas/core/media/vaapi/surface.hpp"

#include "canvas/core/gpu/colorspace.hpp"

#include <cstdint>

#ifdef CANVAS_HAVE_VAAPI
#include <va/va_drmcommon.h>
#else
// Headless/core builds without libva still need the declaration to compile
// the shared_ptr typedefs; the struct is only ever touched inside functions
// that additionally check the macro at runtime.
struct VADRMPRIMESurfaceDescriptor;
#endif

struct AVBufferRef;

namespace canvas::core::vaapi {

// Builds an owning VaapiSurface from an exported descriptor.
//
//   `desc`        the descriptor vaExportSurfaceHandle filled (COMPOSED_LAYERS).
//   `frame_number` source frame number, copied for diagnostics.
//   `spec`        resolved color spec from the decoder (matrix/range).
//   `vendor`      driver identity from identify_vendor (import policy).
//   `pin_ref`     the decoded AVFrame's buf[0]; the returned surface keeps an
//                 av_buffer_ref()'d copy so the VA pool cannot recycle the
//                 surface while our dmabuf/texture is in use. May be null
//                 (surface built without a pin).
//
// Returns null for a descriptor that does not describe a valid NV12 surface
// with the two planes the importer needs.
[[nodiscard]] VaapiSurfacePtr translate_descriptor(const VADRMPRIMESurfaceDescriptor& desc,
                                                   int64_t frame_number,
                                                   const gpu::ColorSpec& spec,
                                                   Vendor vendor,
                                                   AVBufferRef* pin_ref);

}  // namespace canvas::core::vaapi