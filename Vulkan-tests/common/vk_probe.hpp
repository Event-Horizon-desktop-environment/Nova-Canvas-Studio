#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace canvas::vktest {

struct QueueFamilyInfo {
    std::uint32_t index = 0;
    std::uint32_t flags = 0;
    bool video_decode = false;
    bool video_encode = false;
    bool graphics = false;
    bool compute = false;
    std::uint32_t video_codec_operations = 0;
};

struct DeviceInfo {
    std::string name;
    std::string driver_name;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    bool conformance_ok = false;
    std::uint32_t conformance_major = 0;
    std::uint32_t conformance_minor = 0;
    std::uint32_t conformance_subminor = 0;
    std::uint32_t conformance_patch = 0;
    std::vector<QueueFamilyInfo> queue_families;
    std::vector<std::string> extensions;
    bool memory_budget = false;
    bool drm_modifiers = false;
    bool dma_buf_fd = false;
    bool timeline_semaphores = false;
    bool sync2 = false;
    bool push_descriptor = false;
    bool host_query_reset = false;
    bool maintenance4 = false;
    bool external_memory_fd = false;
    bool external_semaphore_fd = false;
    bool has_video_decode_family = false;
    bool has_video_encode_family = false;
};

struct ProbeResult {
    bool ok = false;
    std::string error;
    std::uint32_t instance_api = 0;
    std::vector<DeviceInfo> devices;
};

ProbeResult run_probe();

}
