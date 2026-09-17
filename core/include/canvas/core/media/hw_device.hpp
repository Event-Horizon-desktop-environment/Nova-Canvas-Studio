#pragma once

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace canvas::core {

class HwDeviceManager {
public:
    explicit HwDeviceManager(const char* owner = nullptr);
    ~HwDeviceManager();
    HwDeviceManager(const HwDeviceManager&) = delete;
    HwDeviceManager& operator=(const HwDeviceManager&) = delete;

    const AVBufferRef* device_ctx() const;

    [[nodiscard]] const std::string& device_name() const { return device_name_; }
    [[nodiscard]] const std::string& device_label() const { return device_label_; }
    [[nodiscard]] bool is_hardware() const { return device_ctx_ != nullptr; }

    static void set_preferred_backend(const std::string& backend);
    [[nodiscard]] static const std::string& preferred_backend();

    static void set_preferred_gpu(const std::string& backend,
                                  const std::string& device_arg);
    [[nodiscard]] static const std::string& preferred_device_arg();
    [[nodiscard]] static const std::string& preferred_gpu_backend();

private:
    void init() const;

    mutable AVBufferRef* device_ctx_ = nullptr;
    mutable std::string device_name_;
    mutable std::string device_label_;
    mutable bool tried_ = false;
    std::string owner_;
};

}
