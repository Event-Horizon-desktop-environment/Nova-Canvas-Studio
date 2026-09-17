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
    check(kLookahead <= 1024, "kLookahead is bounded");

    if (failures == 0) {
        std::printf("sync_constants_test: OK\n");
        return 0;
    }
    std::fprintf(stderr, "sync_constants_test: %d failure(s)\n", failures);
    return 1;
}
