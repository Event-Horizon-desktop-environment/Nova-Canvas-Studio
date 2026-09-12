#pragma once
// Headless Vulkan capability probe (Phase 0 — Vulkan-tests).
//
// The whole Vulkan backend is gated on what this probe reports: queue-family
// video capability, video-codec extension set, memory-budget / DRM-modifier /
// dma-buf availability, timeline semaphores, and the conformance version. Its
// tests are the "driver-matrix gating" building block — a decode or composite
// test only runs when the probe says the machine can actually do the work, and
// SKIPs (exit 2) with the owning phase named otherwise. Runs against the real
// loader/driver (no headless lootbox). Qt-free.

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::vktest {

struct QueueFamilyInfo {
    std::uint32_t index = 0;
    std::uint32_t flags = 0;
    bool video_decode = false;   // any decode operation bit set
    bool video_encode = false;   // any encode operation bit set
    bool graphics = false;
    bool compute = false;
    std::uint32_t video_codec_operations = 0;  // raw VkVideoCodecOperationFlagBitsKHR
};

struct DeviceInfo {
    std::string name;
    std::string driver_name;
    std::uint32_t api_version = 0;      // packed (VK_MAKE_API_VERSION)
    std::uint32_t driver_version = 0;   // packed
    bool conformance_ok = false;
    std::uint32_t conformance_major = 0;
    std::uint32_t conformance_minor = 0;
    std::uint32_t conformance_subminor = 0;
    std::uint32_t conformance_patch = 0;
    std::vector<QueueFamilyInfo> queue_families;
    std::vector<std::string> extensions;   // device-extension names
    bool memory_budget = false;            // VK_EXT_memory_budget
    bool drm_modifiers = false;            // VK_EXT_image_drm_format_modifier
    bool dma_buf_fd = false;               // VK_EXT_external_memory_dma_buf
    bool timeline_semaphores = false;      // VK_KHR_timeline_semaphore
    bool sync2 = false;                    // VK_KHR_synchronization2
    bool push_descriptor = false;          // VK_KHR_push_descriptor
    bool host_query_reset = false;         // VK_EXT_host_query_reset
    bool maintenance4 = false;             // VK_KHR_maintenance4
    bool external_memory_fd = false;       // VK_KHR_external_memory_fd
    bool external_semaphore_fd = false;    // VK_KHR_external_semaphore_fd
    bool has_video_decode_family = false;
    bool has_video_encode_family = false;
};

struct ProbeResult {
    bool ok = false;
    std::string error;
    std::uint32_t instance_api = 0;   // packed, what the loader granted
    std::vector<DeviceInfo> devices;  // primary device first
};

// Run the probe. Never fails on a machine that merely lacks a given capability
// (that is what the per-test gates are for); fails only when the loader itself
// cannot provide a functional Vulkan implementation.
ProbeResult run_probe();

}  // namespace canvas::vktest