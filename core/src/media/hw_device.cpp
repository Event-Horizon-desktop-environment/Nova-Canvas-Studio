#include "canvas/core/media/hw_device.hpp"

#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/util/log.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace canvas::core {

namespace {

const char* kProbeOrder[] = {"cuda", "vaapi", "qsv", "vulkan"};

std::string g_preferred_backend;

std::string g_preferred_gpu_backend;
std::string g_preferred_device_arg;

}

void HwDeviceManager::set_preferred_backend(const std::string& backend) {
    g_preferred_backend = backend;
    g_preferred_gpu_backend.clear();
    g_preferred_device_arg.clear();
}

const std::string& HwDeviceManager::preferred_backend() {
    return g_preferred_backend;
}

void HwDeviceManager::set_preferred_gpu(const std::string& backend,
                                        const std::string& device_arg) {
    g_preferred_backend = backend;
    g_preferred_gpu_backend = backend;
    g_preferred_device_arg = device_arg;
}

const std::string& HwDeviceManager::preferred_device_arg() {
    return g_preferred_device_arg;
}

const std::string& HwDeviceManager::preferred_gpu_backend() {
    return g_preferred_gpu_backend;
}

HwDeviceManager::HwDeviceManager(const char* owner) : owner_(owner ? owner : "") {}
HwDeviceManager::~HwDeviceManager() {
    if (device_ctx_) av_buffer_unref(&device_ctx_);
    if (tried_)
        log::log_warning("[hw] device closed last=%s owner=%s",
                         device_name_.empty() ? "none" : device_name_.c_str(),
                         owner_.empty() ? "?" : owner_.c_str());
}

const AVBufferRef* HwDeviceManager::device_ctx() const {
    if (!tried_) {
        tried_ = true;
        init();
    }
    return device_ctx_;
}

void HwDeviceManager::init() const {
    const std::string pinned = g_preferred_backend;

    if (pinned == "software") {
        device_ctx_ = nullptr;
        device_name_.clear();
        log::log_warning("[hw] hardware decode disabled by preference (owner=%s)",
                         owner_.empty() ? "?" : owner_.c_str());
        return;
    }
    std::string order_log;
    std::vector<const char*> order;
    if (!g_preferred_gpu_backend.empty() && !pinned.empty()) {
        order.push_back(g_preferred_backend.c_str());
    } else {
        order.reserve(std::size(kProbeOrder) + 1);
        if (!pinned.empty()) order.push_back(g_preferred_backend.c_str());
        for (const char* name : kProbeOrder) {
            if (pinned.empty() || pinned != name)
                order.push_back(name);
        }
    }
    for (const char* name : order) order_log += std::string(name) + " ";
    log::log_warning("[hw] probing accelerators in order: %s(owner=%s)",
                     order_log.c_str(), owner_.empty() ? "?" : owner_.c_str());
    for (const char* name : order) {
        const AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
        if (type == AV_HWDEVICE_TYPE_NONE) {
            log::log_warning("[hw]   %s: type unavailable", name);
            continue;
        }
        const std::string device =
            (name == g_preferred_gpu_backend) ? g_preferred_device_arg : std::string{};
        const auto t0 = std::chrono::steady_clock::now();
        AVBufferRef* ref = nullptr;
        const int rc = av_hwdevice_ctx_create(
            &ref, type, device.empty() ? nullptr : device.c_str(), nullptr, 0);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        if (rc == 0 && ref) {
            device_ctx_ = ref;
            device_name_ = name;
            device_label_ = gpu_select::gpu_name_for(name, device);
            const std::string gpu_tag = device_label_.empty()
                                            ? std::string{}
                                            : std::string(" gpu=\"") + device_label_ + "\"";
            if (device.empty()) {
                log::log_warning("[hw]   %s: selected in %.1f ms%s", name, ms,
                                 gpu_tag.c_str());
            } else {
                log::log_warning("[hw]   %s: selected in %.1f ms device=%s%s", name,
                                 ms, device.c_str(), gpu_tag.c_str());
            }
            return;
        }
        log::log_warning("[hw]   %s: init FAILED rc=%d in %.1f ms", name, rc, ms);
        if (ref) av_buffer_unref(&ref);
    }
    device_ctx_ = nullptr;
    device_name_.clear();
    log::log_warning("[hw] no accelerator selected; falling back to software decode");
}

}
