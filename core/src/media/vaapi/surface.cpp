#include "canvas/core/media/vaapi/surface.hpp"

#include <unistd.h>

namespace canvas::core::vaapi {

namespace {
// Closes every fd an object list owns. Called by the destructor and cleared
// out of a moved-from surface (its fd ownership already transferred).
void close_objects(std::vector<VaapiObject>& objects) {
    for (auto& obj : objects) {
        if (obj.fd >= 0) {
            ::close(obj.fd);
            obj.fd = -1;
        }
    }
}
}  // namespace

VaapiSurface::~VaapiSurface() {
    close_objects(objects);
}

VaapiSurface::VaapiSurface(VaapiSurface&& other) noexcept {
    *this = std::move(other);
}

VaapiSurface& VaapiSurface::operator=(VaapiSurface&& other) noexcept {
    if (this == &other) return *this;
    close_objects(objects);  // release whatever we currently own
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
    // other's objects/planes are now empty (vector move) → its fds moved here.
    return *this;
}

}  // namespace canvas::core::vaapi