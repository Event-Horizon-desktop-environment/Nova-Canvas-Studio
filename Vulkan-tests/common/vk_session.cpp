// vk_session — live-instance session helper (Phase 0 — Vulkan-tests).
// See vk_session.hpp.

#include "vk_session.hpp"

#include <cstdio>

namespace canvas::vktest {

Session::Session(const std::string& want_name) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "canvas-vk-caps";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        instance = VK_NULL_HANDLE;
        return;
    }
    instance_api = app.apiVersion;

    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) return;
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(instance, &n, all.data());

    VkPhysicalDevice picked = VK_NULL_HANDLE;
    std::string picked_name;
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (picked == VK_NULL_HANDLE) {
            picked = p;
            picked_name = props.deviceName;
        }
        if (!want_name.empty() && want_name == props.deviceName) {
            picked = p;
            picked_name = props.deviceName;
            break;
        }
    }
    physical = picked;
    device_name = picked_name;
}

Session::~Session() {
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
}

}  // namespace canvas::vktest