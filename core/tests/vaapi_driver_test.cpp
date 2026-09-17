#include "canvas/core/media/vaapi/amd.hpp"
#include "canvas/core/media/vaapi/driver.hpp"
#include "canvas/core/media/vaapi/export.hpp"
#include "canvas/core/media/vaapi/nvidia.hpp"
#include "canvas/core/media/vaapi/surface.hpp"

#include <fcntl.h>
#include <unistd.h>

#ifdef CANVAS_HAVE_VAAPI
#include <va/va_drmcommon.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

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

int dup_end_of_pipe() {
    int p[2];
    if (::pipe(p) != 0) return -1;
    ::close(p[0]);
    return p[1];
}

}

int main() {
    namespace v = canvas::core::vaapi;

    using v::Vendor;

    {
        const char* kMesaVendor =
            "Mesa Gallium driver 26.2.2-arch1.1 for AMD Ryzen 9 9900X 12-Core "
            "Processor (radeonsi, raphael_mendocino, ACO, DRM 3.64, 7.2.4-arch1-2)";
        check(v::identify_vendor(kMesaVendor) == Vendor::Amd, "identify_vendor(mesa radeonsi) -> Amd");
        check(v::identify_vendor("radeonsi") == Vendor::Amd, "identify_vendor(\"radeonsi\") -> Amd");
        check(v::identify_vendor("RADEONSI") == Vendor::Amd, "identify_vendor(\"RADEONSI\") case-insensitive");

        check(v::identify_vendor("iHD") == Vendor::Intel, "identify_vendor(\"iHD\") -> Intel");
        check(v::identify_vendor("Intel iHD driver for Intel(R) UHD Graphics 770") == Vendor::Intel,
              "identify_vendor(full iHD vendor string) -> Intel");
        check(v::identify_vendor("i965") == Vendor::Intel, "identify_vendor(\"i965\") -> Intel");

        check(v::identify_vendor("VA-API NVDEC driver [direct backend]") == Vendor::Nvidia,
              "identify_vendor(\"VA-API NVDEC driver\") -> Nvidia");
        check(v::identify_vendor("NVIDIA VA-API v0.3") == Vendor::Nvidia,
              "identify_vendor(\"NVIDIA VA-API v0.3\") -> Nvidia");
        check(v::identify_vendor(std::string(v::nvidia::kDriverName)) == Vendor::Nvidia,
              "identify_vendor(\"nvidia\") -> Nvidia");

        check(v::identify_vendor("") == Vendor::Unknown, "identify_vendor(\"\") -> Unknown");
        check(v::identify_vendor("totally unknown driver") == Vendor::Unknown,
              "identify_vendor(unknown) -> Unknown");
        check(v::nvidia::matches(std::string(kMesaVendor)) == false,
              "nvidia does not match mesa vendor string");
        check(v::amd::matches("VA-API NVDEC driver [direct backend]") == false,
              "amd does not match nvidia vendor string");

        if (failures) goto failed;
    }

    {
        check(std::strcmp(v::vendor_name(Vendor::Amd), "amd") == 0, "vendor_name(Amd) == \"amd\"");
        check(std::strcmp(v::vendor_name(Vendor::Intel), "intel") == 0, "vendor_name(Intel) == \"intel\"");
        check(std::strcmp(v::vendor_name(Vendor::Nvidia), "nvidia") == 0, "vendor_name(Nvidia) == \"nvidia\"");
        check(std::strcmp(v::vendor_name(Vendor::Unknown), "unknown") == 0, "vendor_name(Unknown) == \"unknown\"");
        if (failures) goto failed;
    }

    {
        const auto amd_p = v::import_policy(Vendor::Amd);
        check(amd_p.modifiers_supported, "amd policy keeps modifiers");
        check(!amd_p.force_linear, "amd policy not force-linear");
        check(!amd_p.modifiers_retry_linear, "amd policy no linear retry");

        const auto intel_p = v::import_policy(Vendor::Intel);
        check(intel_p.modifiers_supported, "intel policy keeps modifiers");
        check(intel_p.modifiers_retry_linear, "intel policy retries linear (i965)");

        const auto nvm_p = v::import_policy(Vendor::Nvidia);
        check(!nvm_p.modifiers_supported, "nvidia policy drops modifiers");
        check(nvm_p.force_linear, "nvidia policy forces linear (adapter exports LINEAR)");
        check(!nvm_p.modifiers_retry_linear, "nvidia policy no retry");

        const auto unknown_p = v::import_policy(Vendor::Unknown);
        check(unknown_p.modifiers_supported, "unknown policy allows modifiers");
        check(unknown_p.modifiers_retry_linear, "unknown policy retries linear");

        if (failures) goto failed;
    }

    {
        int fd = dup_end_of_pipe();
        v::VaapiSurface s;
        s.fourcc = v::kDrmFourccNv12;
        s.width = 1920;
        s.height = 1080;
        s.vendor = Vendor::Amd;
        s.objects.push_back({fd, 0});
        s.planes = {{0, 0, 1920}, {0, 1920 * 1080, 1920}};
        check(s.valid(), "constructed surface is valid");

        v::VaapiSurface t = std::move(s);
        check(t.valid(), "moved-to surface valid");
        check(t.objects.size() == 1 && t.objects[0].fd == fd, "fd transferred to moved-to surface");
        check(!s.valid(), "moved-from surface invalid");
        check(s.objects.empty(), "moved-from surface owns no fds");

        {
            int probe = ::fcntl(fd, F_GETFD);
            check(probe >= 0, "fd still open after moved-from destruction");
        }

        v::VaapiSurface u;
        v::VaapiSurface& self = u;
        u = std::move(self);
        check(!u.valid(), "self-move leaves empty surface valid=false");
    }
    {
        int fd = dup_end_of_pipe();
        {
            v::VaapiSurface closing;
            closing.fourcc = v::kDrmFourccNv12;
            closing.width = 128;
            closing.height = 72;
            closing.objects.push_back({fd, 0});
            closing.planes = {{0, 0, 128}, {0, 128 * 72, 128}};
            check(closing.valid(), "surface to be destroyed is valid");
        }
        int probe = ::fcntl(fd, F_GETFD);
        check(probe < 0 && errno == EBADF, "fd closed by destructor");
    }
    {
        v::VaapiSurface empty;
        check(!empty.valid(), "default surface invalid");
        empty.fourcc = v::kDrmFourccNv12;
        empty.width = 640;
        empty.height = 360;
        check(!empty.valid(), "no planes -> invalid");
    }

#ifdef CANVAS_HAVE_VAAPI
    {
        int fd = dup_end_of_pipe();
        VADRMPRIMESurfaceDescriptor desc{};
        desc.fourcc = v::kDrmFourccNv12;
        desc.width = 640;
        desc.height = 360;
        desc.num_objects = 1;
        desc.num_layers = 1;
        desc.objects[0].fd = fd;
        desc.objects[0].drm_format_modifier = 0;
        desc.layers[0].drm_format = v::kDrmFourccNv12;
        desc.layers[0].num_planes = 2;
        desc.layers[0].object_index[0] = 0;
        desc.layers[0].object_index[1] = 0;
        desc.layers[0].offset[0] = 0;
        desc.layers[0].offset[1] = 640 * 360;
        desc.layers[0].pitch[0] = 640;
        desc.layers[0].pitch[1] = 640;

        v::VaapiSurfacePtr sp = v::translate_descriptor(
            desc, 7, {canvas::core::gpu::ColorMatrix::BT601, canvas::core::gpu::ColorRange::Limited}, Vendor::Amd, nullptr);
        check(sp != nullptr, "translate_descriptor returns a surface");
        check(sp && sp->valid(), "translated surface valid");
        check(sp && sp->width == 640 && sp->height == 360, "translated surface geometry");
        check(sp && sp->frame_number == 7, "frame_number copied");
        check(sp && sp->vendor == Vendor::Amd, "vendor copied");
        check(sp && sp->matrix == canvas::core::gpu::ColorMatrix::BT601 && sp->range == canvas::core::gpu::ColorRange::Limited,
              "color spec copied");
        check(sp && sp->planes.size() == 2 && sp->objects.size() == 1, "two planes / one object");
        check(sp && sp->planes[1].offset == 640 * 360, "chroma plane offset");
        check(sp && sp->objects[0].fd >= 0 && sp->objects[0].fd != fd, "descriptor fd dup'd, not adopted");
        int probe = ::fcntl(fd, F_GETFD);
        check(probe >= 0, "original descriptor fd untouched by surface lifetime");
        ::close(fd);
    }
    {
        int fd = dup_end_of_pipe();
        VADRMPRIMESurfaceDescriptor bad{};
        bad.fourcc = 0;
        bad.width = 640;
        bad.height = 360;
        bad.num_objects = 1;
        bad.num_layers = 1;
        bad.objects[0].fd = fd;
        bad.layers[0].num_planes = 2;
        bad.layers[0].object_index[0] = 0;
        bad.layers[0].object_index[1] = 0;
        v::VaapiSurfacePtr sp = v::translate_descriptor(
            bad, 1, {canvas::core::gpu::ColorMatrix::BT709, canvas::core::gpu::ColorRange::Limited}, Vendor::Amd, nullptr);
        check(sp == nullptr, "non-NV12 descriptor rejected");
        int probe = ::fcntl(fd, F_GETFD);
        check(probe >= 0, "rejected descriptor leaves fd ours (not closed, not dup'd)");
        ::close(fd);
    }
#endif

    if (failures) goto failed;

    std::printf("all vaapi_driver checks passed.\n");
    return EXIT_SUCCESS;

failed:
    std::fprintf(stderr, "%d check(s) failed.\n", failures);
    return EXIT_FAILURE;
}
