#pragma once

// Headless GPU discovery for the Settings dialog's GPU picker and the device
// selection it pins into HwDeviceManager / the exporter. No Qt, no libva, no
// CUDA runtime — everything is read from sysfs (the DRM render nodes) plus, for
// NVIDIA, the per-GPU model name the nvidia module publishes under /proc.
//
// The enumerator returns one GpuDevice per render node, each carrying the
// backend that GPU maps to (cuda for NVIDIA, vaapi for AMD/Intel) and the
// device argument that backend's av_hwdevice_ctx_create() wants:
//   - vaapi : the DRM render node path ("/dev/dri/renderD128") — passes the
//     node FFmpeg's VAAPI device init will bind to, so a machine with an
//     NVIDIA dGPU plus an AMD iGPU can target a specific render node.
//   - cuda  : the CUDA device ordinal ("0") — NVIDIA GPUs are assigned
//     ordinals in PCI-slot order, which matches CUDA's default enumeration
//     on every box this targets.
//
// `detect_gpus` takes a filesystem root so the module is fully testable
// against a synthetic sysfs/proc tree; production callers use the default "/".
// Prefer `vendor_name`/`backend_for_vendor`/`device_arg_for` for the pure
// mapping laws (unit-tested); `detect_gpus` wires them to the filesystem.

#include <string>
#include <vector>

namespace canvas::core::gpu_select {

struct GpuDevice {
    std::string pci_slot;    // e.g. "0000:7a:00.0" (stable identity across
                             // boots; keys the persisted settings/hw_gpu value)
    std::string name;        // e.g. "NVIDIA GeForce RTX 5070 Ti" or
                             // "AMD Radeon (Granite Ridge RN)"
    std::string vendor;      // "AMD" | "NVIDIA" | "Intel" (human vendor name)
    std::string backend;     // "cuda" | "vaapi" (backend this GPU drives)
    std::string device_arg;  // render-node path or CUDA ordinal, as above
};

// Pure mapping laws (unit-tested directly):
std::string vendor_name(const std::string& pci_vendor_id);  // "1002" -> "AMD"
std::string backend_for_vendor(const std::string& pci_vendor_id);
    // "10de" -> "cuda"; "1002"/"8086" -> "vaapi"; else "" (unsupported)
std::string device_arg_for(const std::string& backend,
                           const std::string& render_node_name,
                           int cuda_ordinal);
    // vaapi -> "/dev/dri/" + node; cuda -> std::to_string(ordinal)

// Discover every GPU the DRM layer exposes. `root` is the filesystem root to
// scan (default "/" — real system; tests pass a synthetic tree). Returns an
// empty vector when no render nodes exist (headless host / no GPU) — callers
// treat that as "no GPU selection available" and fall back to probe order.
std::vector<GpuDevice> detect_gpus(const std::string& root = "/");

// Resolve the human-readable name of the GPU that `backend`/`device_arg`
// address — the same pair HwDeviceManager passes to av_hwdevice_ctx_create().
// Backend must be "vaapi"/"cuda" (as in GpuDevice::backend); device_arg should
// be the render-node path or CUDA ordinal actually used. Returns the matching
// GpuDevice::name, or "" when nothing matches:
//   - device_arg non-empty: exact match on backend + device_arg.
//   - device_arg empty: the sole GPU with that backend (only unambiguous when
//     exactly one exists, e.g. a single-GPU box); "" on 0 or 2+ candidates.
// `root` is the filesystem root (see detect_gpus; tests pass a synthetic
// tree). This lets the decode path name the physical GPU ("AMD Radeon
// (Granite Ridge)") in logs instead of just the backend ("vaapi").
std::string gpu_name_for(const std::string& backend,
                         const std::string& device_arg,
                         const std::string& root = "/");

}  // namespace canvas::core::gpu_select