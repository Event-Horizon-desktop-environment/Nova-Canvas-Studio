#include "canvas/core/media/vaapi/surface.hpp"

#include <unistd.h>

namespace canvas::core::vaapi {

namespace {
void close_objects(std::vector<VaapiObject>& objects) {
    for (auto& obj : objects) {
        if (obj.fd >= 0) {
            ::close(obj.fd);
            obj.fd = -1;
        }
    }
}
}

VaapiSurface::~VaapiSurface() {
    close_objects(objects);
}

VaapiSurface::VaapiSurface(VaapiSurface&& other) noexcept {
    *this = std::move(other);
}

VaapiSurface& VaapiSurface::operator=(VaapiSurface&& other) noexcept {
    if (this == &other) return *this;
    close_objects(objects);
    fourcc = other.fourcc;
    width = other.width;
    height = other.height;
    frame_number = other.frame_number;
    matrix = other.matrix;
    range = other.range;
    vendor = other.vendor;
    objects = std::move(other.objects);
    planes = std::move(other.planes);
    pin = std::move(other.pin);
    return *this;
}

}
