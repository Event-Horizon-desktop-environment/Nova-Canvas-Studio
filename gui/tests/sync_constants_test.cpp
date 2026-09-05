// Headless unit test for the playback pacing/sync constants.
//
// Qt-free: links only sync_constants.hpp (header-only) so it exercises the
// extraction seam without dragging in Qt. Assertions are plain `if` + return code
// so the test needs no framework (mirrors core/tests style).

#include <cstdio>

#include "features/playback/sync_constants.hpp"

int main() {
    using namespace canvas::gui;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };

    check(kLookahead >= 1, "kLookahead is positive");
    check(kScrubPrecache >= 1, "kScrubPrecache is positive");
    check(kPreviewMaxDim > 0, "kPreviewMaxDim is positive");
    check(kAudioLeadMs >= 0, "kAudioLeadMs is non-negative");
    // The master clock uses kLookahead as the video-ahead-of-audio ceiling; it
    // must be a whole number of frames (already is by type) and bounded so the
    // drop cap never lets video race unboundedly ahead of the audible position.
    check(kLookahead <= 1024, "kLookahead is bounded");

    if (failures == 0) {
        std::printf("sync_constants_test: OK\n");
        return 0;
    }
    std::fprintf(stderr, "sync_constants_test: %d failure(s)\n", failures);
    return 1;
}
