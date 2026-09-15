#include "canvas/core/media/hw_device.hpp"

#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/util/log.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace canvas::core {

namespace {

// Preferred hardware decode device types, ordered by how well they integrate
// with the Linux desktop and by maturity. This list covers NVIDIA (cuda),
// AMD + Intel (vaapi), Intel (qsv), and the cross-vendor Vulkan Video path.
// Each platform/driver makes only the relevant types init successfully, so the
// probe naturally picks whatever the machine can actually do.
const char* kProbeOrder[] = {"cuda", "vaapi", "qsv", "vulkan"};

// User-pinned backend (set_preferred_backend). Empty = default kProbeOrder;
// "software" = skip hardware probing entirely. Consulted by every probe on
// this process, so an app-level preference set once at startup applies to all
// supervisors (playback, thumbs, export) uniformly.
std::string g_preferred_backend;

// Optional per-GPU pin: the specific device argument the pinned backend should
// open (render-node path for vaapi/qsv, CUDA ordinal for cuda). Empty = let
// FFmpeg pick the default device for that backend. Only consulted when the
// type being probed matches g_preferred_gpu_backend.
std::string g_preferred_gpu_backend;
std::string g_preferred_device_arg;

}  // namespace

void HwDeviceManager::set_preferred_backend(const std::string& backend) {
    g_preferred_backend = backend;
    // A bare backend pin clears any previously-set GPU pin so the two never
    // fight: the backend-only path always probes every type with its default
    // device.
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
    // Always-on device teardown note: the HW device is refcounted across every
    // decoder, so this only fires on the last owner (TimelineDecoder/RenderSession
    // close). Right after a project switch both teardown and the next probe show.
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

    // Always-on probe trace: which device types were attempted, in order, and how
    // long each took. A slow one (e.g. CUDA runtime spin-up on an NVA-less box)
    // explains startup stalls; which one actually won explains hw=yes/no in the
    // [dec] lines. Posted once per manager (the first device_ctx() call); the
    // owner tags which subsystem created the context, so a burst of fresh ~300ms
    // probes mid-session is attributable. A user-pinned preference prepends the
    // pinned type so the trace shows the full effective order.
    if (pinned == "software") {
        device_ctx_ = nullptr;
        device_name_.clear();
        log::log_warning("[hw] hardware decode disabled by preference (owner=%s)",
                         owner_.empty() ? "?" : owner_.c_str());
        return;
    }
    // A specific GPU pin is STRICT: it is the only hardware path. The probe
    // list is exactly that one type (+ device), and if it fails to init we fall
    // back to software rather than silently hopping to another accelerator —
    // the whole point of the pin is "use THIS GPU or none".
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
        // A GPU pin applies only to the backend it was set with: the chosen
        // device string (render node / CUDA ordinal) goes to that type's
        // av_hwdevice_ctx_create; every other probe type opens its default.
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
            // Resolve the physical GPU name for logs: with a pinned device we
            // match backend+device_arg exactly; without one we accept the sole
            // GPU of that backend (ambiguous multi-GPU boxes get "").
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
    // No hardware acceleration available; fall back to software decoding.
    device_ctx_ = nullptr;
    device_name_.clear();
    log::log_warning("[hw] no accelerator selected; falling back to software decode");
}

}  // namespace canvas::core
