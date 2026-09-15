#include "canvas/core/media/gpu_select.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>

namespace canvas::core::gpu_select {

namespace {

// PCI vendor id (hex, 4 digits) -> vendor name.
constexpr struct {
    const char* id;
    const char* name;
} kVendors[] = {
    {"1002", "AMD"},
    {"10de", "NVIDIA"},
    {"8086", "Intel"},
};

// A few well-known AMD APU device ids -> marketing name, so the picker shows
// something human ("AMD Radeon (Granite Ridge)") instead of a bare hex. The
// table is intentionally tiny: unknown ids fall back to a generic label.
constexpr struct {
    const char* id;
    const char* name;
} kAmdDevices[] = {
    {"13c0", "Granite Ridge"},  // Ryzen 9000 G-series iGPU (gfx1036)
    {"13c4", "Granite Ridge"},  // dGPU variant, same family
    {"15bf", "Phoenix"},        // Ryzen 7040 series iGPU
    {"164e", "Raphael"},        // Ryzen 7000 series iGPU (gfx1036)
    {"15c9", "Rembrandt"},      // Ryzen 6000 series iGPU
};

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Parse one "KEY=VALUE" line (uevent files). Returns the value, empty if the
// key is absent. Case-insensitive on the key.
std::string uevent_value(const std::string& content, const std::string& key) {
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = to_lower(line.substr(0, eq));
        if (k == key) return line.substr(eq + 1);
    }
    return {};
}

// Read a single small text file, trimming a trailing newline.
std::string read_text(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::string vendor_name(const std::string& pci_vendor_id) {
    for (const auto& v : kVendors) {
        if (to_lower(pci_vendor_id) == v.id) return v.name;
    }
    return {};
}

std::string backend_for_vendor(const std::string& pci_vendor_id) {
    const std::string v = to_lower(pci_vendor_id);
    if (v == "10de") return "cuda";
    if (v == "1002" || v == "8086") return "vaapi";
    return {};
}

std::string device_arg_for(const std::string& backend,
                           const std::string& render_node_name,
                           int cuda_ordinal) {
    if (backend == "vaapi")
        return "/dev/dri/" + render_node_name;
    if (backend == "cuda")
        return std::to_string(cuda_ordinal);
    return {};
}

std::vector<GpuDevice> detect_gpus(const std::string& root) {
    // Build the sysfs path. root is "/" in production; tests pass a synthetic
    // tree (root/class/drm/...). Only render nodes are inventoried — they are
    // the handle both AVHWDeviceType_VAAPI and AVHWDeviceType_CUDA bind to —
    // and deduplicated by PCI slot so one GPU with multiple DRM nodes yields
    // a single picker entry.
    const std::string drm = root + (root == "/" ? "sys/class/drm" : "/class/drm");
    DIR* dir = opendir(drm.c_str());
    if (!dir) return {};
    std::vector<GpuDevice> out;
    std::vector<std::string> done_slots;
    while (const dirent* e = readdir(dir)) {
        const std::string node = e->d_name;
        if (node.rfind("renderD", 0) != 0) continue;
        const std::string uevent = drm + "/" + node + "/device/uevent";
        const std::string content = read_text(uevent);
        if (content.empty()) continue;
        const std::string slot = uevent_value(content, "pci_slot_name");
        if (slot.empty()) continue;
        if (std::find(done_slots.begin(), done_slots.end(), slot) != done_slots.end())
            continue;
        done_slots.push_back(slot);
        const std::string pci_id = uevent_value(content, "pci_id");
        const std::string vendor = pci_id.substr(0, 4);
        const std::string device = pci_id.size() > 5 ? pci_id.substr(5) : "";

        GpuDevice g;
        g.pci_slot = slot;
        g.vendor = vendor_name(vendor);
        g.backend = backend_for_vendor(vendor);
        if (g.backend.empty()) continue;  // unsupported vendor: skip the entry

        if (to_lower(vendor) == "10de") {
            // NVIDIA publishes a friendly model name in
            // /proc/driver/nvidia/gpus/<slot>/information (first line:
            // "Model: NEVIN NVIDIA GeForce ..."). Fall back to a generic label.
            const std::string info =
                root + (root == "/" ? "proc/driver/nvidia/gpus/"
                                    : "/proc/driver/nvidia/gpus/") + slot + "/information";
            const std::string text = read_text(info);
            g.name = "NVIDIA GPU";
            if (!text.empty()) {
                const auto pos = text.find("Model:");
                if (pos != std::string::npos) {
                    std::string m = text.substr(pos + 6);
                    m.erase(0, m.find_first_not_of(" \t"));
                    const auto nl = m.find_first_of("\r\n");
                    if (nl != std::string::npos) m.erase(nl);
                    if (!m.empty()) g.name = m;
                }
            }
        } else {
            std::string model;
            for (const auto& d : kAmdDevices) {
                if (to_lower(device) == d.id) {
                    model = d.name;
                    break;
                }
            }
            g.name = vendor_name(vendor);
            if (!model.empty())
                g.name += " Radeon (" + model + ")";
            else if (!device.empty())
                g.name += " GPU (PCI " + to_lower(device) + ")";
            else
                g.name += " GPU";
        }

        // CUDA ordinals are assigned in PCI-slot order here so the picker's
        // "GPU 1" matches device_arg "0" deterministically.
        g.device_arg = device_arg_for(g.backend, node,
                                      g.backend == "cuda" ? static_cast<int>(out.size()) : 0);
        out.push_back(std::move(g));
    }
    closedir(dir);

    // Stable order: PCI slot, so the picker is deterministic between runs.
    std::sort(out.begin(), out.end(),
              [](const GpuDevice& a, const GpuDevice& b) { return a.pci_slot < b.pci_slot; });
    // Re-derive CUDA ordinals after sorting (the sort above can reorder the
    // ordinal-compressed counter).
    int cuda_idx = 0;
    for (auto& g : out) {
        if (g.backend == "cuda")
            g.device_arg = std::to_string(cuda_idx++);
    }
    return out;
}

std::string gpu_name_for(const std::string& backend,
                         const std::string& device_arg,
                         const std::string& root) {
    if (backend != "vaapi" && backend != "cuda") return {};
    const auto gpus = detect_gpus(root);
    if (gpus.empty()) return {};
    std::string match;
    if (!device_arg.empty()) {
        for (const auto& g : gpus) {
            if (g.backend == backend && g.device_arg == device_arg) return g.name;
        }
        return {};
    }
    // No device arg: unambiguous only when this backend has a single GPU.
    for (const auto& g : gpus) {
        if (g.backend != backend) continue;
        if (!match.empty()) return {};  // second candidate — ambiguous
        match = g.name;
    }
    return match;
}

}  // namespace canvas::core::gpu_select