#include "canvas/core/media/hw_device.hpp"

namespace canvas::core {

namespace {

// Preferred hardware decode device types, ordered by how well they integrate
// with the Linux desktop and by maturity. This list covers NVIDIA (cuda),
// AMD + Intel (vaapi), Intel (qsv), and the cross-vendor Vulkan Video path.
// Each platform/driver makes only the relevant types init successfully, so the
// probe naturally picks whatever the machine can actually do.
const char* kProbeOrder[] = {"cuda", "vaapi", "qsv", "vulkan"};

}  // namespace

HwDeviceManager::HwDeviceManager() = default;
HwDeviceManager::~HwDeviceManager() {
    if (device_ctx_) av_buffer_unref(&device_ctx_);
}

const AVBufferRef* HwDeviceManager::device_ctx() const {
    if (!tried_) {
        tried_ = true;
        init();
    }
    return device_ctx_;
}

void HwDeviceManager::init() const {
    for (const char* name : kProbeOrder) {
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
        if (type == AV_HWDEVICE_TYPE_NONE) continue;

        AVBufferRef* ref = nullptr;
        if (av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0) == 0 && ref) {
            device_ctx_ = ref;
            device_name_ = name;
            return;
        }
        if (ref) av_buffer_unref(&ref);
    }
    // No hardware acceleration available; fall back to software decoding.
    device_ctx_ = nullptr;
    device_name_.clear();
}

}  // namespace canvas::core
