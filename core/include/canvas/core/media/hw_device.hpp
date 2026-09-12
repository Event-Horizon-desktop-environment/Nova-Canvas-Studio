#pragma once

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace canvas::core {

// Result of probing/creating a hardware decode device. `device_ctx` may be
// null when no usable hardware acceleration is available (e.g. no GPU/driver),
// in which case the caller should fall back to software decoding.
// A single instance should be shared app-wide so every decoder reuses the same
// GPU device instead of creating one per decode session.
class HwDeviceManager {
public:
    // `owner` names the subsystem that created this manager (e.g. "playback",
    // "thumbs", "render", "main") so the probe/teardown census in the log can
    // attribute which client created (and destroyed) a CUDA context. A fresh
    // ~300ms "cuda: selected" probe spiking mid-session points at whatever owner
    // is constructing new managers on that path. Default is empty (unknown).
    explicit HwDeviceManager(const char* owner = nullptr);
    ~HwDeviceManager();
    HwDeviceManager(const HwDeviceManager&) = delete;
    HwDeviceManager& operator=(const HwDeviceManager&) = delete;

    // Lazily create a shared hardware device, probing the requested device
    // types (in order) and returning the first one that initializes on this
    // machine. Falls back to software (device_ctx == nullptr) if none work.
    const AVBufferRef* device_ctx() const;

    // Human-readable device type string (e.g. "cuda", "vaapi", "qsv",
    // "vulkan") or an empty string when software decoding. Useful for logging.
    [[nodiscard]] const std::string& device_name() const { return device_name_; }
    [[nodiscard]] bool is_hardware() const { return device_ctx_ != nullptr; }

private:
    void init() const;

    mutable AVBufferRef* device_ctx_ = nullptr;
    mutable std::string device_name_;
    mutable bool tried_ = false;
    std::string owner_;
};

}  // namespace canvas::core
