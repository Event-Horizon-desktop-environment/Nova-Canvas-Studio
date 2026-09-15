// Headless DeliverSettingsModel tests (splitplan Phase 31). Compiles
// deliver_settings_model.cpp directly into the binary so the module is verified
// exactly as shipped; the Qt-free seam is enforced by the build (a stray <Q...>
// include breaks this target on purpose).
//
// Covers the container<->codec restriction policy the Deliver settings panel
// applies to its combo boxes, the encoder/preset lists, and the bitrate-field
// visibility law:
//   * every video codec the policy allows for a container is also a member of
//     canvas::core::deliver_video_codecs() (display-name parity);
//   * WebM exposes only AV1 video and Opus/Vorbis audio;
//   * MXF/IMF are H.26x-only video and PCM-only audio;
//   * image-sequence formats reduce video to Uncompressed;
//   * matching is case-insensitive on the container name;
//   * bitrate visibility: constant-QP / VBR-quality hide both fields,
//     VBR-target shows both (relabelling the single field "Target (Kbps)"),
//     constant-bitrate shows only the single "Bit Rate" field.

#include "features/deliver/deliver_settings_model.hpp"

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/media/hw_device.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace canvas::gui;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool subset_of(const std::vector<std::string>& inner,
               const std::vector<std::string>& outer) {
    for (const std::string& s : inner)
        if (!contains(outer, s)) return false;
    return true;
}

void test_presets() {
    const auto& presets = deliver_model::preset_names();
    CHECK(!presets.empty());
    CHECK(contains(presets, "Custom Export"));
    CHECK(contains(presets, "H.264 MP4 Web"));
    CHECK(contains(presets, "YouTube 1080p"));
}

void test_encoder_backends_wrap_core() {
    CHECK(deliver_model::encoder_backends() == canvas::core::deliver_encoders());
}

void test_available_encoder_backends() {
    // Auto + CPU are never filtered out and keep their plain labels.
    const auto choices = deliver_model::available_encoder_backends();
    CHECK(choices.size() >= 2);
    if (choices.size() < 2) return;
    CHECK(choices.at(0).label == "Auto" && choices.at(0).key == "Auto");
    CHECK(choices.at(1).label == "CPU" && choices.at(1).key == "CPU");

    // Every key is a canonical core name; labels carry the encoder family.
    const std::vector<std::string> core_names = canvas::core::deliver_encoders();
    size_t vendor_entries = 0;
    for (const auto& c : choices) {
        CHECK(std::find(core_names.begin(), core_names.end(), c.key) != core_names.end());
        if (c.key == "Auto" || c.key == "CPU") continue;
        ++vendor_entries;
        if (c.key == "NVIDIA") CHECK(c.label == "NVIDIA NVENC");
        else if (c.key == "AMD") CHECK(c.label == "AMD VAAPI");
        else if (c.key == "Intel") CHECK(c.label == "Intel QSV");
    }

    // The vendor entries mirror the GPUs actually present on this machine
    // (same detection the Settings dialog uses): each entry must correspond to
    // a detected vendor GPU, and no missing vendor may be listed.
    const auto gpus = canvas::core::gpu_select::detect_gpus();
    std::vector<std::string> detected;
    for (const auto& g : gpus) detected.push_back(g.vendor);
    for (const auto& c : choices) {
        const std::string key = c.key;
        if (key == "Auto" || key == "CPU") continue;
        CHECK(std::find(detected.begin(), detected.end(), key) != detected.end());
    }
    (void)vendor_entries;  // keep the counter meaningful if a compiler ever prunes it
}

void test_video_codecs_are_display_names() {
    const auto all = canvas::core::deliver_video_codecs();
    CHECK(subset_of(deliver_model::video_codecs_for_format("MKV"), all));
    CHECK(subset_of(deliver_model::video_codecs_for_format("MP4"), all));
    CHECK(subset_of(deliver_model::video_codecs_for_format("QuickTime"), all));
    CHECK(subset_of(deliver_model::video_codecs_for_format("WebM"), all));
    CHECK(subset_of(deliver_model::video_codecs_for_format("AVI"), all));
    CHECK(subset_of(deliver_model::video_codecs_for_format("MXF OP-Atom"), all));
}

void test_audio_codecs_are_display_names() {
    const auto all = canvas::core::deliver_audio_codecs();
    CHECK(subset_of(deliver_model::audio_codecs_for_format("MP4"), all));
    CHECK(subset_of(deliver_model::audio_codecs_for_format("MKV"), all));
    CHECK(subset_of(deliver_model::audio_codecs_for_format("WebM"), all));
    CHECK(subset_of(deliver_model::audio_codecs_for_format("AVI"), all));
    CHECK(subset_of(deliver_model::audio_codecs_for_format("MXF OP1A"), all));
}

void test_webm_policy() {
    const auto vcs = deliver_model::video_codecs_for_format("WebM");
    CHECK(vcs.size() == 1 && vcs.at(0) == "AV1");
    const auto acs = deliver_model::audio_codecs_for_format("WebM");
    CHECK(acs.size() == 2 && acs.at(0) == "Opus" && acs.at(1) == "Vorbis");
}

void test_mp4_policy() {
    // MP4 exposes H.26x + AV1; ProRes/FFV1/etc. are rejected up front.
    const auto vcs = deliver_model::video_codecs_for_format("MP4");
    CHECK(vcs.size() == 3);
    if (vcs.size() == 3) {
        CHECK(vcs.at(0) == "H.264" && vcs.at(1) == "H.265" && vcs.at(2) == "AV1");
    }
    CHECK(!contains(vcs, "Apple ProRes"));
    const auto acs = deliver_model::audio_codecs_for_format("MP4");
    CHECK(contains(acs, "AAC"));
    CHECK(contains(acs, "FLAC"));
}

void test_mkv_exposes_every_codec() {
    const auto vcs = deliver_model::video_codecs_for_format("MKV");
    for (const std::string& c : canvas::core::deliver_video_codecs())
        CHECK(contains(vcs, c));
    const auto acs = deliver_model::audio_codecs_for_format("MKV");
    for (const std::string& c : canvas::core::deliver_audio_codecs())
        CHECK(contains(acs, c));
}

void test_quicktime_policy() {
    const auto vcs = deliver_model::video_codecs_for_format("QuickTime");
    CHECK(contains(vcs, "Apple ProRes"));
    CHECK(!contains(vcs, "AV1"));
    // Lower-case 'mov' form is accepted too.
    CHECK(vcs == deliver_model::video_codecs_for_format("mov"));
}

void test_avi_policy() {
    const auto vcs = deliver_model::video_codecs_for_format("AVI");
    CHECK(contains(vcs, "FFV1"));
    CHECK(!contains(vcs, "AV1"));
    const auto acs = deliver_model::audio_codecs_for_format("AVI");
    CHECK(acs.size() == 2 && contains(acs, "PCM") && contains(acs, "MP3"));
    CHECK(!contains(acs, "AAC"));
}

void test_mxf_policy() {
    for (const std::string& fmt : {"MXF OP-Atom", "MXF OP1A"}) {
        const auto vcs = deliver_model::video_codecs_for_format(fmt);
        CHECK(vcs.size() == 2 && vcs.at(0) == "H.264" && vcs.at(1) == "H.265");
        const auto acs = deliver_model::audio_codecs_for_format(fmt);
        CHECK(acs.size() == 1 && acs.at(0) == "PCM");
    }
}

void test_image_sequence_policy() {
    for (const std::string& fmt : {"PNG", "DPX", "EXR", "TIFF", "WebP", "GIF"}) {
        const auto vcs = deliver_model::video_codecs_for_format(fmt);
        CHECK(vcs.size() == 1 && vcs.at(0) == "Uncompressed");
    }
    // "JPEG 2000" the container hits the image-sequence branch through "jpeg".
    CHECK(deliver_model::video_codecs_for_format("JPEG 2000") ==
          deliver_model::video_codecs_for_format("PNG"));
}

void test_case_insensitive() {
    CHECK(deliver_model::video_codecs_for_format("webm") ==
          deliver_model::video_codecs_for_format("WebM"));
    CHECK(deliver_model::audio_codecs_for_format("MPEG-2") ==
          deliver_model::audio_codecs_for_format("mpeg"));
}

void test_bitrate_visibility() {
    // RateControl: 0=ConstantQP, 1=VBRQuality, 2=VBRTargetKbps, 3=ConstantBitrate.
    const auto qp = deliver_model::bitrate_visibility(0);
    CHECK(!qp.show_bitrate && !qp.show_max);
    CHECK(std::string(qp.bitrate_label) == "Bit Rate");

    const auto vbrq = deliver_model::bitrate_visibility(1);
    CHECK(!vbrq.show_bitrate && !vbrq.show_max);

    const auto vbrt = deliver_model::bitrate_visibility(2);
    CHECK(vbrt.show_bitrate && vbrt.show_max);
    CHECK(std::string(vbrt.bitrate_label) == "Target (Kbps)");

    const auto cbr = deliver_model::bitrate_visibility(3);
    CHECK(cbr.show_bitrate && !cbr.show_max);
    CHECK(std::string(cbr.bitrate_label) == "Bit Rate");

    // Unknown index behaves like the quality modes (fields hidden).
    const auto bogus = deliver_model::bitrate_visibility(7);
    CHECK(!bogus.show_bitrate && !bogus.show_max);
}

// available_encoder_backends() with a Preferences GPU pin set: the list
// collapses to that GPU's vendor only (an AMD-pinned user sees no NVIDIA
// NVENC), and widens back to every detected vendor once the pin is cleared.
void test_available_backends_respect_gpu_pin() {
    using canvas::core::HwDeviceManager;

    auto vendor_keys = []() {
        std::vector<std::string> keys;
        for (const auto& c : deliver_model::available_encoder_backends())
            if (c.key != "Auto" && c.key != "CPU") keys.push_back(c.key);
        return keys;
    };

    // Start unpinned so the unpinned baseline matches detect_gpus().
    HwDeviceManager::set_preferred_gpu("", "");
    const auto unpinned = vendor_keys();
    const bool have_amd = contains(unpinned, "AMD");
    const bool have_nvidia = contains(unpinned, "NVIDIA");
    const bool have_intel = contains(unpinned, "Intel");

    // Amber: AMD VAAPI only -> AMD kept, NVIDIA dropped (when present).
    HwDeviceManager::set_preferred_gpu("vaapi", "/dev/dri/renderD128");
    auto keys = vendor_keys();
    if (have_amd) CHECK(contains(keys, "AMD"));
    if (have_nvidia) CHECK(!contains(keys, "NVIDIA"));
    if (have_intel) CHECK(!contains(keys, "Intel"));

    // NVIDIA NVENC only -> NVIDIA kept, AMD dropped (when present).
    HwDeviceManager::set_preferred_gpu("cuda", "0");
    keys = vendor_keys();
    if (have_nvidia) CHECK(contains(keys, "NVIDIA"));
    if (have_amd) CHECK(!contains(keys, "AMD"));
    if (have_intel) CHECK(!contains(keys, "Intel"));

    // Clearing the pin restores every detected vendor.
    HwDeviceManager::set_preferred_gpu("", "");
    keys = vendor_keys();
    if (have_amd) CHECK(contains(keys, "AMD"));
    if (have_nvidia) CHECK(contains(keys, "NVIDIA"));
    if (have_intel) CHECK(contains(keys, "Intel"));
}

// Strict GPU pin law: video_encoder_name demotes any hardware backend naming a
// GPU different from the pinned one to software (sw_fallback set), while the
// matching backend is preserved. No pin -> all hardware backends honored.
void test_gpu_pin_demotes_foreign_backends() {
    using canvas::core::EncoderBackend;
    using canvas::core::HwDeviceManager;
    using canvas::core::VideoCodec;

    auto encode = [](VideoCodec c, EncoderBackend b) {
        bool sw = false;
        const std::string n = canvas::core::video_encoder_name(c, b, "MKV", &sw);
        return std::make_pair(n, sw);
    };

    // No pin: every hardware backend survives.
    HwDeviceManager::set_preferred_backend("");
    HwDeviceManager::set_preferred_gpu("", "");
    {
        auto [nvid, sw] = encode(VideoCodec::H264, EncoderBackend::NVIDIA);
        CHECK(nvid == "h264_nvenc" && !sw);
        auto [amdx, sw2] = encode(VideoCodec::H264, EncoderBackend::AMD);
        CHECK(amdx == "h264_vaapi" && !sw2);
    }

    // AMD pinned: NVENC and QSV demote to software; VAAPI survives.
    HwDeviceManager::set_preferred_gpu("vaapi", "/dev/dri/renderD128");
    {
        auto [nvid, sw] = encode(VideoCodec::H264, EncoderBackend::NVIDIA);
        CHECK(nvid == "libx264" && sw);
        auto [amdx, sw2] = encode(VideoCodec::H264, EncoderBackend::AMD);
        CHECK(amdx == "h264_vaapi" && !sw2);
        auto [itl, sw3] = encode(VideoCodec::H265, EncoderBackend::Intel);
        CHECK(itl == "libx265" && sw3);
        auto [nvidh, sw4] = encode(VideoCodec::H265, EncoderBackend::NVIDIA);
        CHECK(nvidh == "libx265" && sw4);
    }

    // NVIDIA pinned: VAAPI and QSV demote; NVENC survives.
    HwDeviceManager::set_preferred_gpu("cuda", "0");
    {
        auto [amdx, sw] = encode(VideoCodec::H264, EncoderBackend::AMD);
        CHECK(amdx == "libx264" && sw);
        auto [nvid, sw2] = encode(VideoCodec::H264, EncoderBackend::NVIDIA);
        CHECK(nvid == "h264_nvenc" && !sw2);
        auto [itl, sw3] = encode(VideoCodec::AV1, EncoderBackend::Intel);
        CHECK(itl == "libsvtav1" && sw3);
    }

    // Clear the pin so later tests in this process aren't affected.
    HwDeviceManager::set_preferred_gpu("", "");
}

}  // namespace

int main() {
    test_presets();
    test_encoder_backends_wrap_core();
    test_available_encoder_backends();
    test_video_codecs_are_display_names();
    test_audio_codecs_are_display_names();
    test_webm_policy();
    test_mp4_policy();
    test_mkv_exposes_every_codec();
    test_quicktime_policy();
    test_avi_policy();
    test_mxf_policy();
    test_image_sequence_policy();
    test_case_insensitive();
    test_bitrate_visibility();
    test_available_backends_respect_gpu_pin();
    test_gpu_pin_demotes_foreign_backends();

    if (g_failures == 0) {
        std::printf("deliver_settings_model_test: ALL PASS\n");
        return 0;
    }
    std::printf("deliver_settings_model_test: %d FAILURE(S)\n", g_failures);
    return 1;
}