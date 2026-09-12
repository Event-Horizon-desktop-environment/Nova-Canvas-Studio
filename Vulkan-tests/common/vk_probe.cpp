// Headless Vulkan capability probe — implementation (Phase 0 — Vulkan-tests).
// See vk_probe.hpp for the contract. Qt-free; links Vulkan::Vulkan only.

#include "vk_probe.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>

namespace canvas::vktest {

namespace {

// Try to enable the video-queue extensions at instance level so the queue
// family probe can read real video-codec operation bits. Missing extensions
// are simply skipped (the device list still reports their absence).
void enable_available_extensions(std::vector<const char*>& names) {
    static const char* const kVideoInstanceExts[] = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
    };
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
        return;
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()) != VK_SUCCESS)
        return;
    for (const char* wanted : kVideoInstanceExts) {
        if (std::any_of(props.begin(), props.end(), [wanted](const VkExtensionProperties& p) {
                return std::string(p.extensionName) == wanted;
            }))
            names.push_back(wanted);
    }
}

}  // namespace

ProbeResult run_probe() {
    ProbeResult out;

    out.instance_api = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enum_api = {};
    if (vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion") != nullptr)
        enum_api = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enum_api == nullptr || enum_api(&out.instance_api) != VK_SUCCESS)
        out.instance_api = 0;

    std::vector<const char*> inst_exts;
    enable_available_extensions(inst_exts);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "canvas-vulkan-tests";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = out.instance_api >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3
                                                            : VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
    ci.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        out.error = "vkCreateInstance failed; is there a Vulkan loader + ICD?";
        return out;
    }

    out.ok = true;
    out.instance_api = app.apiVersion;

    std::uint32_t dev_count = 0;
    if (vkEnumeratePhysicalDevices(inst, &dev_count, nullptr) != VK_SUCCESS || dev_count == 0)
        return out;
    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(inst, &dev_count, devices.data());

    out.devices.reserve(devices.size());
    for (std::uint32_t d = 0; d < dev_count; ++d) {
        VkPhysicalDevice phys = devices[d];
        DeviceInfo info;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        info.name = props.deviceName;
        info.api_version = props.apiVersion;
        info.driver_version = props.driverVersion;

        // Driver-name + conformance come via the PROPERTIES2 chain.
        VkPhysicalDeviceDriverPropertiesKHR driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
        VkPhysicalDeviceVulkan13Properties v13{};
        v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &driver;
        driver.pNext = &v13;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        info.driver_name = driver.driverName;
        if (driver.conformanceVersion.major || driver.conformanceVersion.minor ||
            driver.conformanceVersion.subminor || driver.conformanceVersion.patch) {
            info.conformance_ok = true;
            info.conformance_major = driver.conformanceVersion.major;
            info.conformance_minor = driver.conformanceVersion.minor;
            info.conformance_subminor = driver.conformanceVersion.subminor;
            info.conformance_patch = driver.conformanceVersion.patch;
        }

        // Queue families + their video-codec operation bits (only meaningful
        // when the video extensions were enabled at instance level).
        std::uint32_t fam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties2(phys, &fam_count, nullptr);
        std::vector<VkQueueFamilyProperties2> families(fam_count);
        for (auto& f : families) {
            f.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            f.pNext = nullptr;
        }
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(fam_count);
        for (std::uint32_t i = 0; i < fam_count; ++i) {
            video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            video[i].pNext = nullptr;
            families[i].pNext = &video[i];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(phys, &fam_count, families.data());
        info.queue_families.reserve(fam_count);
        for (std::uint32_t i = 0; i < fam_count; ++i) {
            QueueFamilyInfo q;
            q.index = i;
            q.flags = families[i].queueFamilyProperties.queueFlags;
            q.video_decode = (video[i].videoCodecOperations &
                              (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR |
                               VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR |
                               VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR |
                               VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR)) != 0;
            q.video_encode = (video[i].videoCodecOperations &
                              (VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR |
                               VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR |
                               VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)) != 0;
            q.graphics = (families[i].queueFamilyProperties.queueFlags &
                          VK_QUEUE_GRAPHICS_BIT) != 0;
            q.compute = (families[i].queueFamilyProperties.queueFlags &
                         VK_QUEUE_COMPUTE_BIT) != 0;
            info.has_video_decode_family |= q.video_decode;
            info.has_video_encode_family |= q.video_encode;
            info.queue_families.push_back(q);
        }

        // Device extension set.
        std::uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data());
        info.extensions.reserve(ext_count);
        for (const auto& e : exts) {
            const std::string name = e.extensionName;
            info.extensions.push_back(name);
            if (name == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) info.memory_budget = true;
            if (name == VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) info.drm_modifiers = true;
            if (name == VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) info.dma_buf_fd = true;
            if (name == VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) info.timeline_semaphores = true;
            if (name == VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) info.sync2 = true;
            if (name == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) info.push_descriptor = true;
            if (name == VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) info.host_query_reset = true;
            if (name == VK_KHR_MAINTENANCE_4_EXTENSION_NAME) info.maintenance4 = true;
            if (name == VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) info.external_memory_fd = true;
            if (name == VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) info.external_semaphore_fd = true;
        }

        // Core-promotion: several interop extensions became core by Vulkan
        // 1.1/1.2/1.3, so a conformant driver may not list them as extensions.
        // OR in the version floor so the report says "usable", not "named".
        if (info.api_version >= VK_API_VERSION_1_1) {
            info.external_memory_fd = true;
            info.external_semaphore_fd = true;
        }
        if (info.api_version >= VK_API_VERSION_1_2) {
            info.timeline_semaphores = true;
            info.host_query_reset = true;
        }
        if (info.api_version >= VK_API_VERSION_1_3) {
            info.sync2 = true;
            info.maintenance4 = true;
        }

        out.devices.push_back(std::move(info));
    }

    vkDestroyInstance(inst, nullptr);

    // Primary device = first decoded (driver order usually puts the discrete
    // GPU first). The report keeps the full list for later machine-matrix work.
    return out;
}

}  // namespace canvas::vktest