#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include "canvas/core/gpu/colorspace.hpp"

#include <cstdint>

#ifdef CANVAS_HAVE_VAAPI
#include <va/va_drmcommon.h>
#else
struct VADRMPRIMESurfaceDescriptor;
#endif

struct AVBufferRef;

namespace canvas::core::vaapi {

[[nodiscard]] VaapiSurfacePtr translate_descriptor(const VADRMPRIMESurfaceDescriptor& desc,
                                                   int64_t frame_number,
                                                   const gpu::ColorSpec& spec,
                                                   Vendor vendor,
                                                   AVBufferRef* pin_ref);

}
