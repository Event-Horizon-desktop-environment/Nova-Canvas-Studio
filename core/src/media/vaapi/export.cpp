#include "canvas/core/media/vaapi/export.hpp"

#ifdef CANVAS_HAVE_VAAPI

#include "canvas/core/util/log.hpp"

#include <cerrno>
#include <unistd.h>

extern "C" {
#include <libavutil/buffer.h>
}

namespace canvas::core::vaapi {

VaapiSurfacePtr translate_descriptor(const VADRMPRIMESurfaceDescriptor& desc,
                                     const int64_t frame_number,
                                     const gpu::ColorSpec& spec,
                                     const Vendor vendor,
                                     AVBufferRef* const pin_ref) {
    if (desc.width == 0 || desc.height == 0 || desc.num_layers == 0 || desc.num_objects == 0) {
        ::canvas::core::log::log_warning(
            "[vaapi] translate_descriptor: degenerate descriptor (w=%u h=%u layers=%u objects=%u)",
            desc.width, desc.height, desc.num_layers, desc.num_objects);
        return nullptr;
    }
    if (desc.fourcc != kDrmFourccNv12) {
        ::canvas::core::log::log_warning("[vaapi] translate_descriptor: fourcc=%04x not NV12 (frame=%lld)",
                                         desc.fourcc, static_cast<long long>(frame_number));
        return nullptr;
    }

    // Composed-layers export produces exactly one NV12 layer with two planes.
    // (SEPARATE_LAYERS would give one layer per plane — the decode worker
    // always exports COMPOSED_LAYERS, so anything else here is unexpected.)
    const auto& layer = desc.layers[0];
    if (desc.num_layers != 1 || layer.num_planes < 2) {
        ::canvas::core::log::log_warning(
            "[vaapi] translate_descriptor: unexpected layer layout (layers=%u planes=%u)",
            desc.num_layers, layer.num_planes);
        return nullptr;
    }

    auto surf = std::make_shared<VaapiSurface>();
    surf->fourcc = desc.fourcc;
    surf->width = static_cast<int>(desc.width);
    surf->height = static_cast<int>(desc.height);
    surf->frame_number = frame_number;
    surf->matrix = spec.matrix;
    surf->range = spec.range;
    surf->vendor = vendor;

    // Pin the source VA surface first: if any fd dup below fails we bail with
    // nothing held (the caller falls back to CPU), and if it succeeds the pin
    // keeps the surface reserved for as long as our dmabufs are alive.
    if (pin_ref) {
        AVBufferRef* const pin_copy = av_buffer_ref(pin_ref);
        if (pin_copy) {
            surf->pin = std::shared_ptr<const void>(
                pin_copy, [](const void* p) {
                    auto* ref = const_cast<AVBufferRef*>(static_cast<const AVBufferRef*>(p));
                    av_buffer_unref(&ref);
                });
        }
        if (!surf->pin) {
            ::canvas::core::log::log_warning(
                "[vaapi] translate_descriptor: no pin ref (frame=%lld) — surface may recycle early",
                static_cast<long long>(frame_number));
        }
    }

    // Duplicate every object fd so the surface owns its own dmabufs. The
    // descriptor's fds belong to the driver and close when vaExportSurfaceHandle
    // returns / the driver unrefs; we must not reuse or close them.
    surf->objects.reserve(desc.num_objects);
    for (std::uint32_t i = 0; i < desc.num_objects; ++i) {
        const int dup_fd = ::dup(desc.objects[i].fd);
        if (dup_fd < 0) {
            ::canvas::core::log::log_warning("[vaapi] translate_descriptor: dup(fd=%d) FAILED (errno=%d)",
                                             desc.objects[i].fd, errno);
            return nullptr;  // ~VaapiSurface closes the fds dup'd so far
        }
        surf->objects.push_back(VaapiObject{dup_fd, desc.objects[i].drm_format_modifier});
    }

    // Record the importer's per-plane geometry. object_index is kept as-is so
    // it indexes surf->objects one-to-one with the descriptor's objects array.
    for (std::uint32_t p = 0; p < layer.num_planes; ++p) {
        if (layer.object_index[p] >= desc.num_objects) {
            ::canvas::core::log::log_warning(
                "[vaapi] translate_descriptor: plane %u object_index %u out of range (%u)",
                p, layer.object_index[p], desc.num_objects);
            return nullptr;
        }
        surf->planes.push_back(VaapiPlane{layer.object_index[p], layer.offset[p], layer.pitch[p]});
    }

    if (!surf->valid()) {
        ::canvas::core::log::log_warning(
            "[vaapi] translate_descriptor: surface not valid (fourcc=%04x objects=%zu planes=%zu)",
            surf->fourcc, surf->objects.size(), surf->planes.size());
        return nullptr;
    }

    CANVAS_LOG("[vaapi] translate_descriptor OK frame=%lld %dx%d vendor=%s fds=%zu y_pitch=%u uv_pitch=%u",
               static_cast<long long>(frame_number), surf->width, surf->height, vendor_name(surf->vendor),
               surf->objects.size(), surf->planes[0].pitch, surf->planes[1].pitch);
    return surf;
}

}  // namespace canvas::core::vaapi

#else  // !CANVAS_HAVE_VAAPI

#include "canvas/core/util/log.hpp"

namespace canvas::core::vaapi {

VaapiSurfacePtr translate_descriptor(const VADRMPRIMESurfaceDescriptor&,
                                     const int64_t,
                                     const gpu::ColorSpec&,
                                     const Vendor,
                                     AVBufferRef*) {
    ::canvas::core::log::log_warning("[vaapi] translate_descriptor: built without libva (CANVAS_HAVE_VAAPI=0)");
    return nullptr;
}

}  // namespace canvas::core::vaapi

#endif  // CANVAS_HAVE_VAAPI