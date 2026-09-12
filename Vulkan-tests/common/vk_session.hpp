#pragma once
// Headless Vulkan session helper for capability tests (Phase 0 — Vulkan-tests).
//
// The probe reports device capabilities from a transient instance. Tests that
// need to call PHYSICAL-device queries (image format support, video profiles,
// queue family properties, memory types) need a live VkInstance/VkPhysicalDevice
// handle. This helper recreates the instance and finds the physical device that
// matches the probe's chosen (primary) device, so the capability tests stay
// small and never duplicate instance/device selection. Qt-free.
//
// If the capability tests go further and need a real VkDevice (Phases P-C/P-D),
// extend this with a queue-enabled device ctor — the session is the seam.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace canvas::vktest {

struct Session {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::string device_name;
    std::uint32_t instance_api = 0;

    // Create an instance and select the physical device whose name matches
    // `want_name` ("" = first enumerated). Destroys the instance on destruction.
    explicit Session(const std::string& want_name = "");
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

}  // namespace canvas::vktest