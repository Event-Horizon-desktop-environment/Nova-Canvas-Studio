#pragma once

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

    explicit Session(const std::string& want_name = "");
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

}