// Headless GPU-discovery tests for canvas::core::gpu_select. The pure mapping
// laws (vendor_name, backend_for_vendor, device_arg_for) are exercised
// directly; detect_gpus is fed a synthetic sysfs/proc tree under a temp dir
// so the tests neither need (nor accidentally touch) the real / proc.

#include "canvas/core/media/gpu_select.hpp"

#include <cstdio>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "canvas/core/util/log.hpp"

namespace canvas::core::gpu_select {
namespace {

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    if (!out) return false;
    out << content;
    return out.good();
}

bool write_uevent(const std::string& root, const std::string& drm_node,
                  const std::string& driver, const std::string& pci_id,
                  const std::string& pci_slot) {
    const std::string base = root + "/class/drm/" + drm_node + "/device";
    for (const std::string& dir : {root + "/class",
                                   root + "/class/drm",
                                   root + "/class/drm/" + drm_node,
                                   base}) {
        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    const std::string content =
        "DRIVER=" + driver + "\nMAJOR=226\n"
        "PCI_ID=" + pci_id + "\nPCI_SLOT_NAME=" + pci_slot + "\n";
    return write_file(base + "/uevent", content);
}

// FATAL-guard: every test here is pure or self-contained, so a helper failure
// is a bug in the harness — abort loudly rather than silently succeeding.
bool ensure(bool ok, const char* what) {
    if (!ok) log::log_error("gpu_select_test harness failed: %s", what);
    return ok;
}

bool test_mapping_laws() {
    bool ok = true;

    // vendor_name
    ensure(vendor_name("1002") == "AMD", "vendor 1002 -> AMD");
    ensure(vendor_name("10de") == "NVIDIA", "vendor 10de -> NVIDIA");
    ensure(vendor_name("8086") == "Intel", "vendor 8086 -> Intel");
    ensure(vendor_name("1234") == "", "unknown vendor -> empty");

    // backend_for_vendor
    ensure(backend_for_vendor("10de") == "cuda", "NVIDIA -> cuda");
    ensure(backend_for_vendor("1002") == "vaapi", "AMD -> vaapi");
    ensure(backend_for_vendor("8086") == "vaapi", "Intel -> vaapi");
    ensure(backend_for_vendor("1234") == "", "unknown vendor -> no backend");

    // device_arg_for
    ensure(device_arg_for("vaapi", "renderD128", 0) == "/dev/dri/renderD128",
           "vaapi render-node arg");
    ensure(device_arg_for("vaapi", "renderD129", 0) == "/dev/dri/renderD129",
           "vaapi render-node arg (other node)");
    ensure(device_arg_for("cuda", "renderD129", 0) == "0",
           "cuda ordinal 0");
    ensure(device_arg_for("cuda", "renderD129", 3) == "3",
           "cuda ordinal 3");
    ensure(device_arg_for("", "renderD128", 0) == "",
           "unknown backend -> empty arg");

    return ok;
}

bool test_detect_amd_only() {
    const std::string root = "/tmp/canvas_gpu_test_amd";
    const bool made = mkdir(root.c_str(), 0755) == 0 || errno == EEXIST;
    if (!ensure(made, "mkdtemp root")) return false;
    const bool u =
        write_uevent(root, "renderD128", "amdgpu", "1002:13C0", "0000:7a:00.0");
    if (!ensure(u, "amd uevent")) return false;

    const auto gpus = detect_gpus(root);
    bool ok = ensure(gpus.size() == 1, "one AMD GPU found");
    if (gpus.size() == 1) {
        const auto& g = gpus[0];
        ok &= ensure(g.pci_slot == "0000:7a:00.0", "amd slot");
        ok &= ensure(g.backend == "vaapi", "amd backend");
        ok &= ensure(g.vendor == "AMD", "amd vendor");
        ok &= ensure(g.device_arg == "/dev/dri/renderD128", "amd device arg");
        ok &= ensure(g.name.find("AMD") == 0, "amd name prefix");
        ok &= ensure(g.name.find("Granite Ridge") != std::string::npos,
                     "amd model name (Granite Ridge)");
    }
    return ok;
}

bool test_detect_nvidia_amd_order() {
    const std::string root = "/tmp/canvas_gpu_test_mixed";
    const bool made = mkdir(root.c_str(), 0755) == 0 || errno == EEXIST;
    if (!ensure(made, "mkdtemp root")) return false;
    // NOTE: inserted in reverse PCI order on purpose — the sort must fix it.
    const bool a = write_uevent(root, "renderD128", "amdgpu", "1002:13C0",
                                "0000:7a:00.0");
    const bool n = write_uevent(root, "renderD129", "nvidia", "10DE:2C05",
                                "0000:01:00.0");
    if (!ensure(a && n, "mixed uevents")) return false;

    // Simulate the real /proc nvidia model file:
    const std::string info_dir = root + "/proc/driver/nvidia/gpus/0000:01:00.0";
    {
        const std::string parents[] = {root + "/proc",
                                       root + "/proc/driver",
                                       root + "/proc/driver/nvidia",
                                       root + "/proc/driver/nvidia/gpus",
                                       info_dir};
        for (const auto& dir : parents) {
            if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    const std::string model = "Model: NVIDIA GeForce RTX 5070 Ti\nBus Location: 0000:01:00.0\n";
    if (!ensure(write_file(info_dir + "/information", model), "nvidia model file"))
        return false;

    const auto gpus = detect_gpus(root);
    bool ok = ensure(gpus.size() == 2, "two GPUs found");
    if (gpus.size() == 2) {
        // PCI-slot order: nvidia (01) sorts before amd (7a).
        ok &= ensure(gpus[0].backend == "cuda" && gpus[1].backend == "vaapi",
                     "sorted by pci slot (cuda then vaapi)");
        ok &= ensure(gpus[0].vendor == "NVIDIA" && gpus[1].vendor == "AMD",
                     "vendor names");
        ok &= ensure(gpus[0].pci_slot == "0000:01:00.0"
                         && gpus[1].pci_slot == "0000:7a:00.0",
                     "slot order");
        ok &= ensure(gpus[0].device_arg == "0", "nvidia cuda ordinal 0");
        ok &= ensure(gpus[0].name == "NVIDIA GeForce RTX 5070 Ti",
                     "nvidia model name parsed");
        ok &= ensure(gpus[1].device_arg == "/dev/dri/renderD128",
                     "amd render-node arg after sort");
    }
    return ok;
}

bool test_unsupported_vendor_skipped() {
    const std::string root = "/tmp/canvas_gpu_test_unknown";
    const bool made = mkdir(root.c_str(), 0755) == 0 || errno == EEXIST;
    if (!ensure(made, "mkdtemp root")) return false;
    if (!ensure(write_uevent(root, "renderD130", "virtio-gpu", "1234:5678",
                             "0000:00:02.0"),
                "unknown vendor uevent"))
        return false;
    const auto gpus = detect_gpus(root);
    return ensure(gpus.empty(), "unsupported vendor skipped");
}

bool test_empty_sysfs() {
    const std::string root = "/tmp/canvas_gpu_test_empty";
    const bool made = mkdir(root.c_str(), 0755) == 0 || errno == EEXIST;
    if (!ensure(made, "mkdtemp root")) return false;
    const auto gpus = detect_gpus(root);
    return ensure(gpus.empty(), "empty tree -> no GPUs");
}

bool test_gpu_name_for() {
    bool ok = true;

    // Nonexistent sysfs -> no name.
    ensure(gpu_name_for("vaapi", "/dev/dri/renderD128",
                        "/tmp/canvas_gpu_test_does_not_exist") == "",
           "no sysfs -> no name");

    // AMD-only: exact device_arg match, and the sole-GPU backend match.
    const std::string root_a = "/tmp/canvas_gpu_test_name_amd";
    if (!ensure(mkdir(root_a.c_str(), 0755) == 0 || errno == EEXIST,
                "mkdtemp amd")) return false;
    if (!ensure(write_uevent(root_a, "renderD128", "amdgpu", "1002:13C0",
                             "0000:7a:00.0"),
                "amd uevent")) return false;
    ensure(gpu_name_for("vaapi", "/dev/dri/renderD128", root_a).find("AMD") == 0,
           "exact device_arg match");
    ensure(gpu_name_for("vaapi", "/dev/dri/renderD129", root_a) == "",
           "non-matching device_arg -> empty");
    ensure(gpu_name_for("cuda", "0", root_a) == "",
           "missing backend -> empty");
    ensure(gpu_name_for("vaapi", "", root_a).find("AMD") == 0,
           "sole AMD GPU matched without device arg");

    // Mixed NVIDIA+AMD: sole-match per backend resolves each.
    const std::string root_m = "/tmp/canvas_gpu_test_name_mixed";
    if (!ensure(mkdir(root_m.c_str(), 0755) == 0 || errno == EEXIST,
                "mkdtemp mixed")) return false;
    if (!ensure(write_uevent(root_m, "renderD128", "amdgpu", "1002:13C0",
                             "0000:7a:00.0") &&
                    write_uevent(root_m, "renderD129", "nvidia", "10DE:2C05",
                                 "0000:01:00.0"),
                "mixed uevents")) return false;
    const std::string info_dir =
        root_m + "/proc/driver/nvidia/gpus/0000:01:00.0";
    {
        const std::string parents[] = {root_m + "/proc",
                                       root_m + "/proc/driver",
                                       root_m + "/proc/driver/nvidia",
                                       root_m + "/proc/driver/nvidia/gpus",
                                       info_dir};
        for (const auto& dir : parents) {
            if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    if (!ensure(write_file(info_dir + "/information",
                           "Model: NVIDIA GeForce RTX 5070 Ti\n"),
                "nvidia model file")) return false;
    ensure(gpu_name_for("vaapi", "/dev/dri/renderD128", root_m).find("AMD") == 0,
           "mixed: AMD by device_arg");
    ensure(gpu_name_for("cuda", "0", root_m) == "NVIDIA GeForce RTX 5070 Ti",
           "mixed: NVIDIA by device_arg");
    ensure(gpu_name_for("vaapi", "", root_m).find("AMD") == 0,
           "mixed: sole AMD resolved without device arg");

    // Two AMD GPUs: no-device_arg must be ambiguous ("").
    const std::string root_2 = "/tmp/canvas_gpu_test_name_2amd";
    if (!ensure(mkdir(root_2.c_str(), 0755) == 0 || errno == EEXIST,
                "mkdtemp 2amd")) return false;
    if (!ensure(write_uevent(root_2, "renderD128", "amdgpu", "1002:13C0",
                             "0000:01:00.0") &&
                    write_uevent(root_2, "renderD130", "amdgpu", "1002:13C0",
                                 "0000:7a:00.0"),
                "two amd uevents")) return false;
    ensure(gpu_name_for("vaapi", "", root_2) == "",
           "two AMD GPUs without device_arg -> ambiguous");
    ensure(gpu_name_for("vaapi", "/dev/dri/renderD130", root_2).find("AMD") == 0,
           "two AMD GPUs WITH device_arg -> resolved");

    return ok;
}

}  // namespace

// Test driver: runs every check, exits 0 on all-clear and 1 on any failure.
int run_gpu_select_tests() {
    bool all = true;
    all &= test_mapping_laws();
    all &= test_detect_amd_only();
    all &= test_detect_nvidia_amd_order();
    all &= test_unsupported_vendor_skipped();
    all &= test_empty_sysfs();
    all &= test_gpu_name_for();
    if (!all) log::log_error("gpu_select: FAILED");
    return all ? 0 : 1;
}

}  // namespace canvas::core::gpu_select

int main() {
    return canvas::core::gpu_select::run_gpu_select_tests();
}