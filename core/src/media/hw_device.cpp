#include "canvas/core/media/hw_device.hpp"

#include "canvas/core/util/log.hpp"

#include <chrono>

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
    // Always-on device teardown note: the HW device is refcounted across every
    // decoder, so this only fires on the last owner (TimelineDecoder/RenderSession
    // close). Right after a project switch both teardown and the next probe show.
    if (tried_)
        log::log_warning("[hw] device closed last=%s", device_name_.empty() ? "none" : device_name_.c_str());
}

const AVBufferRef* HwDeviceManager::device_ctx() const {
    if (!tried_) {
        tried_ = true;
        init();
    }
    return device_ctx_;
}

void HwDeviceManager::init() const {
    // Always-on probe trace: which device types were attempted, in order, and how
    // long each took. A slow one (e.g. CUDA runtime spin-up on an NVA-less box)
    // explains startup stalls; which one actually won explains hw=yes/no in the
    // [dec] lines. Posted once per process (the first device_ctx() call).
    log::log_warning("[hw] probing accelerators in order: cuda vaapi qsv vulkan");
    for (const char* name : kProbeOrder) {
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
        if (type == AV_HWDEVICE_TYPE_NONE) {
            log::log_warning("[hw]   %s: type unavailable", name);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        AVBufferRef* ref = nullptr;
        const int rc = av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        if (rc == 0 && ref) {
            device_ctx_ = ref;
            device_name_ = name;
            log::log_warning("[hw]   %s: selected in %.1f ms", name, ms);
            return;
        }
        log::log_warning("[hw]   %s: init FAILED rc=%d in %.1f ms", name, rc, ms);
        if (ref) av_buffer_unref(&ref);
    }
    // No hardware acceleration available; fall back to software decoding.
    device_ctx_ = nullptr;
    device_name_.clear();
    log::log_warning("[hw] no accelerator selected; falling back to software decode");
}

}  // namespace canvas::core
