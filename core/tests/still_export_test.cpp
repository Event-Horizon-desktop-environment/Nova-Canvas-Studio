#include "canvas/core/export/deliver_preset.hpp"

#include <cstdio>
#include <string>

using namespace canvas::core;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

void test_predicates() {
    check(!scope_is_still(RenderScope::SingleClip), "SingleClip is not still");
    check(!scope_is_still(RenderScope::IndividualClips), "IndividualClips is not still");
    check(scope_is_still(RenderScope::Still), "Still is still");
    check(!scope_is_still(RenderScope::FrameSequence), "FrameSequence is not still");

    check(scope_is_sequence(RenderScope::FrameSequence), "FrameSequence is sequence");
    check(!scope_is_sequence(RenderScope::Still), "Still is not sequence");

    check(scope_is_image(RenderScope::Still), "Still is an image scope");
    check(scope_is_image(RenderScope::FrameSequence), "FrameSequence is an image scope");
    check(!scope_is_image(RenderScope::SingleClip), "SingleClip is not an image scope");
    check(!scope_is_image(RenderScope::IndividualClips), "IndividualClips is not an image scope");
}

void test_still_paths() {
    check(still_output_path("out") == "out.png", "bare stem gains .png");
    check(still_output_path("out.png") == "out.png", "png kept");
    check(still_output_path("out.jpg") == "out.png", "extension replaced");
    check(still_output_path("/a/b/clip.mov") == "/a/b/clip.png", "path extension replaced");
    check(still_output_path("/a.b/out") == "/a.b/out.png", "dot in directory is not an extension");
    check(still_output_path("frame.v2.png") == "frame.v2.png", "only the last extension is stripped");
}

void test_sequence_paths() {
    check(sequence_output_path("out", 1) == "out_00001.png", "sequence starts at 00001");
    check(sequence_output_path("out", 42) == "out_00042.png", "sequence zero-pads to five");
    check(sequence_output_path("out.png", 1234) == "out_01234.png", "sequence replaces extension");
    check(sequence_output_path("/a/b/out.mov", 7) == "/a/b/out_00007.png", "sequence keeps path");
    check(sequence_output_path("out", 0) == "out_00001.png", "index < 1 clamps to 1");
    check(sequence_output_path("out", -5) == "out_00001.png", "negative index clamps to 1");
}

}

int main() {
    test_predicates();
    test_still_paths();
    test_sequence_paths();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
