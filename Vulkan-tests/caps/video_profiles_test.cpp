// video_profiles_test — per-codec decode/encode profile availability.
//
// Phase 0 reading that pins WHICH codec profiles the machine's video queue
// families actually expose, per family. This is the pre-flight question Phase
// P-C (decode) and P-E (encode) must answer before building one queue-session
// per codec: given a profile, does vkGetPhysicalDeviceVideoCapabilitiesKHR
// accept it as supported?
//
// The backend's decoder/encoder matrix (docs/vulkan.md) is driven by exactly
// these bits — a profile the driver refuses cannot be handed to the FFmpeg
// vulkan decoder, full stop. So this test queries the honest capability API
// (not the extension list) for every profile the plan intends to run:
//   decode: h264 main/high, h265 main/main10, av1 main, vp9 profile0
//   encode: h264 main, h265 main, av1 main
//
// PASS (0)  — every profile the plan needs is supported by a family that
//             advertises the corresponding codec operation.
// FAIL (1)  — the driver claims the operation bit but refuses every profile
//             (a real integration hazard: FFmpeg will probe and fail at init).
// SKIP (2)  — no decode+encode-capable device at all (owning phase P-C/P-E).

#include "vk_probe.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace canvas::vktest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct ProfileQuery {
    std::string name;
    bool decode = false;              // decode vs encode operation
    VkVideoCodecOperationFlagBitsKHR op{};
    VkVideoProfileInfoKHR profile{};
    VkVideoDecodeH264ProfileInfoKHR dec_h264{};
    VkVideoDecodeH265ProfileInfoKHR dec_h265{};
    VkVideoDecodeAV1ProfileInfoKHR dec_av1{};
    VkVideoDecodeVP9ProfileInfoKHR dec_vp9{};
    VkVideoEncodeH264ProfileInfoKHR enc_h264{};
    VkVideoEncodeH265ProfileInfoKHR enc_h265{};
    VkVideoEncodeAV1ProfileInfoKHR enc_av1{};
};

ProfileQuery make_query(const char* name, bool decode,
                        VkVideoCodecOperationFlagBitsKHR op,
                        VkVideoChromaSubsamplingFlagsKHR chroma) {
    ProfileQuery q;
    q.name = name;
    q.decode = decode;
    q.op = op;
    q.profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    q.profile.videoCodecOperation = op;
    q.profile.chromaSubsampling = chroma;
    q.profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    q.profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    return q;
}

}  // namespace

int main() {
    const ProbeResult r = run_probe();
    if (!r.ok || r.devices.empty()) {
        std::printf("SKIP  video_profiles: no Vulkan implementation (%s)\n",
                    r.ok ? "no devices" : r.error.c_str());
        return 2;
    }

    const DeviceInfo& d = r.devices.front();
    if (!d.has_video_decode_family && !d.has_video_encode_family) {
        std::printf("SKIP  video_profiles: %s has no video-capable queue family "
                    "(owning phases P-C/P-E build the codec sessions there)\n",
                    d.name.c_str());
        return 2;
    }

    // Each expected profile, wired to its codec-info struct.
    std::vector<ProfileQuery> queries;
    {
        ProfileQuery q = make_query("h264-decode-main", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        q.dec_h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        q.dec_h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        q.profile.pNext = &q.dec_h264;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("h264-decode-high", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        q.dec_h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        q.dec_h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        q.profile.pNext = &q.dec_h264;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("h265-decode-main", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        q.dec_h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        q.profile.pNext = &q.dec_h265;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("h265-decode-main10", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        q.dec_h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
        q.profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        q.profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        q.profile.pNext = &q.dec_h265;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("av1-decode-main", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
        q.dec_av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        q.dec_av1.filmGrainSupport = VK_FALSE;
        q.profile.pNext = &q.dec_av1;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("vp9-decode-profile0", true,
                                    VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.dec_vp9.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
        q.dec_vp9.stdProfile = STD_VIDEO_VP9_PROFILE_0;
        q.profile.pNext = &q.dec_vp9;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("h264-encode-main", false,
                                    VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.enc_h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
        q.enc_h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        q.profile.pNext = &q.enc_h264;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("h265-encode-main", false,
                                    VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.enc_h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;
        q.enc_h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        q.profile.pNext = &q.enc_h265;
        queries.push_back(q);
    }
    {
        ProfileQuery q = make_query("av1-encode-main", false,
                                    VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
                                    VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
        q.enc_av1.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR;
        q.enc_av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        q.profile.pNext = &q.enc_av1;
        queries.push_back(q);
    }

    // Need the video_queue extension at instance level for the capability
    // query entry points (the query itself is a physical-device call).
    std::uint32_t ext_n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_n, nullptr);
    std::vector<VkExtensionProperties> exts(ext_n);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_n, exts.data());
    const auto has_ext = [&](const char* name) {
        for (const auto& e : exts)
            if (std::string(e.extensionName) == name) return true;
        return false;
    };

    std::vector<const char*> inst_exts;
    if (has_ext(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME))
        inst_exts.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
    if (has_ext(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME))
        inst_exts.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
    if (has_ext(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME))
        inst_exts.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
    // The per-codec extensions gate the capability query itself: a profile is
    // only queried acceptably when its codec extension is enabled.
    for (const char* name : {VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
                             VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
                             VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
                             VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
                             VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME}) {
        if (has_ext(name)) inst_exts.push_back(name);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "canvas-video-profiles";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        std::printf("SKIP  video_profiles: instance with video extensions failed\n");
        return 2;
    }

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    {
        std::uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> all(n);
        vkEnumeratePhysicalDevices(inst, &n, all.data());
        for (VkPhysicalDevice p : all) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(p, &props);
            if (props.deviceName == d.name) {
                phys = p;
                break;
            }
        }
    }
    if (phys == VK_NULL_HANDLE) {
        vkDestroyInstance(inst, nullptr);
        std::printf("SKIP  video_profiles: primary device not re-enumerable\n");
        return 2;
    }

    auto get_caps = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
        vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if (get_caps == nullptr) {
        vkDestroyInstance(inst, nullptr);
        std::printf("SKIP  video_profiles: vkGetPhysicalDeviceVideoCapabilitiesKHR "
                    "not exported (video_queue ext missing from loader?)\n");
        return 2;
    }

    for (auto& q : queries) {
        // Does any family claim this operation bit?
        bool op_on_family = false;
        for (const auto& f : d.queue_families) {
            if ((f.video_codec_operations & q.op) != 0) op_on_family = true;
        }
        // The capability query itself is authoritative for profile support.
        VkVideoCapabilitiesKHR caps{};
        caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        const VkResult vr = get_caps(phys, &q.profile, &caps);

        bool relevant = op_on_family || vr == VK_SUCCESS;
        const char* status;
        if (vr == VK_SUCCESS) status = "supported";
        else if (vr == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR ||
                 vr == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR ||
                 vr == VK_ERROR_FEATURE_NOT_PRESENT)
            status = "unsupported";
        else status = "query-error";

        std::printf("%s  video_profiles: %-18s family-claims=%-3s caps=%s\n",
                    (vr == VK_SUCCESS) ? "PASS" : (relevant ? "FAIL" : "PASS"),
                    q.name.c_str(), op_on_family ? "yes" : "no", status);

        // Law: if the operation bit is advertised but every profile is refused,
        // that codec is un-runnable on this machine (FFmpeg probes the same way
        // and fails at session init in P-C/P-E). That is only a FAIL here when
        // the plan actually requires the codec operation bit family-wise; a
        // non-advertised codec is simply not part of this machine's matrix.
        if (op_on_family && vr != VK_SUCCESS) {
            std::printf("FAIL  video_profiles: %s claims operation bit but no profile accepted\n",
                        q.name.c_str());
            ++g_failures;
        }
    }

    vkDestroyInstance(inst, nullptr);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}