#include "canvas/core/export/vaapi_encode.hpp"

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

}

int main() {
    using canvas::core::vaapi_rate_control_from;

    check(canvas::core::is_vaapi_codec("h264_vaapi"), "is_vaapi_codec(h264_vaapi)");
    check(canvas::core::is_vaapi_codec("hevc_vaapi"), "is_vaapi_codec(hevc_vaapi)");
    check(canvas::core::is_vaapi_codec("av1_vaapi"), "is_vaapi_codec(av1_vaapi)");
    check(!canvas::core::is_vaapi_codec("libx264"), "not vaapi for libx264");
    check(!canvas::core::is_vaapi_codec("h264_nvenc"), "not vaapi for nvenc");

    auto rc = vaapi_rate_control_from("h264_vaapi", 23, "constqp", 0);
    check(rc.rc_mode == "CQP", "constqp maps to rc_mode=CQP");
    check(rc.qp == 23, "constqp qp == crf");
    check(rc.global_quality == 23, "constqp global_quality == crf");

    rc = vaapi_rate_control_from("h264_vaapi", 18, "auto", 0);
    check(rc.rc_mode == "CQP", "auto maps to rc_mode=CQP (not ICQ: radeonsi has none)");
    check(rc.qp == 18, "auto qp == crf");
    check(rc.global_quality == 18, "auto global_quality == crf");

    rc = vaapi_rate_control_from("h264_vaapi", 20, "vbr_target", 0);
    check(rc.rc_mode == "CQP", "h264 vbr_target+crf maps to rc_mode=CQP (no ICQ on radeonsi)");

    rc = vaapi_rate_control_from("hevc_vaapi", 20, "vbr_target", 8000);
    check(rc.rc_mode == "QVBR", "hevc vbr_target+bitrate maps to rc_mode=QVBR");

    rc = vaapi_rate_control_from("av1_vaapi", 20, "vbr", 4000);
    check(rc.rc_mode == "QVBR", "av1 vbr+bitrate maps to rc_mode=QVBR");

    rc = vaapi_rate_control_from("hevc_vaapi", 20, "vbr_target", 0);
    check(rc.rc_mode == "CQP", "hevc vbr_target w/o bitrate falls back to CQP");
    rc = vaapi_rate_control_from("av1_vaapi", 20, "vbr", 0);
    check(rc.rc_mode == "CQP", "av1 vbr w/o bitrate falls back to CQP");

    rc = vaapi_rate_control_from("av1_vaapi", 23, "constqp", 0);
    check(rc.qp == 115, "av1 constqp qp scaled by 5");
    check(rc.global_quality == 115, "av1 constqp global_quality scaled by 5");

    rc = vaapi_rate_control_from("h264_vaapi", -1, "cbr", 8000);
    check(rc.rc_mode == "CBR", "crf<0 + cbr maps to rc_mode=CBR");
    rc = vaapi_rate_control_from("hevc_vaapi", -1, "vbr_target", 8000);
    check(rc.rc_mode == "VBR", "crf<0 + vbr_target maps to rc_mode=VBR");
    rc = vaapi_rate_control_from("hevc_vaapi", -1, "auto", 8000);
    check(rc.rc_mode.empty(), "crf<0 + auto leaves rc_mode unset");
    check(rc.qp == -1 && rc.global_quality == -1, "bitrate modes keep quality unset");

    rc = vaapi_rate_control_from("libx264", 23, "constqp", 0);
    check(rc.rc_mode.empty(), "libx264 not mapped");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed.\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all vaapi_encode checks passed.\n");
    return EXIT_SUCCESS;
}
