// VAAPI rate-control mapping tests (canvas/core/export/qsv_encode.hpp) —
// the §8.1 fix in docs/qsv.md: VAAPI encoders have no `crf` option, so the
// engine's NVENC-shaped quality push silently no-ops. This pins the mapping
// that translates crf/rc-mode intent onto VAAPI's rc_mode/global_quality knobs.
// Pure function — no libavcodec needed, so it runs anywhere canvas_core builds.

#include "canvas/core/export/qsv_encode.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

}  // namespace

int main() {
    using canvas::core::qsv_rate_control_from;

    check(canvas::core::is_qsv_codec("h264_qsv"), "is_qsv_codec(h264_qsv)");
    check(canvas::core::is_qsv_codec("hevc_qsv"), "is_qsv_codec(hevc_qsv)");
    check(canvas::core::is_qsv_codec("av1_qsv"), "is_qsv_codec(av1_qsv)");
    check(!canvas::core::is_qsv_codec("libx264"), "not qsv for libx264");
    check(!canvas::core::is_qsv_codec("h264_nvenc"), "not qsv for nvenc");

    // constqp -> CQP with qp + global_quality pinned.
    auto rc = qsv_rate_control_from("h264_qsv", 23, "constqp", 0);
    check(rc.rc_mode == "CQP", "constqp maps to rc_mode=CQP");
    check(rc.qp == 23, "constqp qp == crf");
    check(rc.global_quality == 23, "constqp global_quality == crf");

    // auto (unspelled) -> ICQ, the universal CRF analog.
    rc = qsv_rate_control_from("h264_qsv", 18, "auto", 0);
    check(rc.rc_mode == "ICQ", "auto maps to rc_mode=ICQ");
    check(rc.global_quality == 18, "auto global_quality == crf");

    // vbr / vbr_target -> QVBR for HEVC/AV1, ICQ for H.264 (h264 has no QVBR).
    rc = qsv_rate_control_from("hevc_qsv", 20, "vbr_target", 0);
    check(rc.rc_mode == "QVBR", "hevc vbr_target maps to rc_mode=QVBR");
    rc = qsv_rate_control_from("av1_qsv", 20, "vbr", 0);
    check(rc.rc_mode == "QVBR", "av1 vbr maps to rc_mode=QVBR");
    rc = qsv_rate_control_from("h264_qsv", 20, "vbr_target", 0);
    check(rc.rc_mode == "ICQ", "h264 vbr_target maps to rc_mode=ICQ (no QVBR)");

    // AV1 quality scale: 0-255 vs 0-51 -> crf*5.
    rc = qsv_rate_control_from("av1_qsv", 23, "constqp", 0);
    check(rc.qp == 115, "av1 constqp qp scaled by 5");
    check(rc.global_quality == 115, "av1 constqp global_quality scaled by 5");

    // Bitrate-driven (crf<0) modes.
    rc = qsv_rate_control_from("h264_qsv", -1, "cbr", 8000);
    check(rc.rc_mode == "CBR", "crf<0 + cbr maps to rc_mode=CBR");
    rc = qsv_rate_control_from("hevc_qsv", -1, "vbr_target", 8000);
    check(rc.rc_mode == "VBR", "crf<0 + vbr_target maps to rc_mode=VBR");
    rc = qsv_rate_control_from("hevc_qsv", -1, "auto", 8000);
    check(rc.rc_mode.empty(), "crf<0 + auto leaves rc_mode unset");
    check(rc.qp == -1 && rc.global_quality == -1, "bitrate modes keep quality unset");

    // Non-VAAPI codecs get no mapping at all (exporter falls back to crf push).
    rc = qsv_rate_control_from("libx264", 23, "constqp", 0);
    check(rc.rc_mode.empty(), "libx264 not mapped");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed.\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all qsv_encode checks passed.\n");
    return EXIT_SUCCESS;
}