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

}  // namespace

int main() {
    test_presets();
    test_encoder_backends_wrap_core();
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

    if (g_failures == 0) {
        std::printf("deliver_settings_model_test: ALL PASS\n");
        return 0;
    }
    std::printf("deliver_settings_model_test: %d FAILURE(S)\n", g_failures);
    return 1;
}