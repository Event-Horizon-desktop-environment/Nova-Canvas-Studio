#pragma once

#include <string>
#include <vector>

namespace canvas::core::gpu_select {

inline constexpr const char* kCpuSentinel = "cpu";

struct GpuDevice {
    std::string pci_slot;
    std::string name;
    std::string vendor;
    std::string backend;
    std::string device_arg;
};

std::string vendor_name(const std::string& pci_vendor_id);
std::string backend_for_vendor(const std::string& pci_vendor_id);
std::string device_arg_for(const std::string& backend,
                           const std::string& render_node_name,
                           int cuda_ordinal);

std::vector<GpuDevice> detect_gpus(const std::string& root = "/");

std::string cpu_name(const std::string& root = "/");

std::string gpu_name_for(const std::string& backend,
                         const std::string& device_arg,
                         const std::string& root = "/");

}
