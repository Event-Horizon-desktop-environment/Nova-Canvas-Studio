#include "Widgets/timeline_widget.hpp"
#include "features/color/mini_timeline_strip.hpp"

#include <QApplication>
#include <QGraphicsScene>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

using namespace canvas::gui;

namespace {

constexpr std::uint64_t kPoolNs = 0x8000000000000000ULL;
constexpr std::uint64_t kProjectNs = 0xE000000000000000ULL;
constexpr std::uint64_t kSourcePreviewId = 0xF000000000000001ULL;

bool is_pool_thumb_id(std::uint64_t id) {
    return (id & kProjectNs) == kPoolNs;
}
bool equals_project_region(std::uint64_t id) { return (id & kProjectNs) == kProjectNs; }

}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    TimelineWidget timeline;
    MiniTimelineStrip ministrip;
    (void)timeline;
    (void)ministrip;

    int failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        if (!ok) {
            failures += 1;
            std::printf("FAIL: %s\n", what);
        }
    };

    expect("kTimelineThumbNs / kMiniStripThumbNs distinct",
           kTimelineThumbNs != kMiniStripThumbNs);
    expect("timeline ns is a single high bit",
           (kTimelineThumbNs & (kTimelineThumbNs - 1)) == 0);
    expect("ministrip ns is a single high bit",
           (kMiniStripThumbNs & (kMiniStripThumbNs - 1)) == 0);

    expect("timeline ns outside pool region", (kTimelineThumbNs & kPoolNs) == 0);
    expect("timeline ns outside project region", (kTimelineThumbNs & kProjectNs) == 0);
    expect("timeline ns != source-preview sentinel", kTimelineThumbNs != kSourcePreviewId);
    expect("ministrip ns outside pool region", (kMiniStripThumbNs & kPoolNs) == 0);
    expect("ministrip ns outside project region", (kMiniStripThumbNs & kProjectNs) == 0);
    expect("ministrip ns != source-preview sentinel", kMiniStripThumbNs != kSourcePreviewId);

    {
        std::unordered_set<std::uint64_t> seen;
        std::uint64_t timeline_id = kTimelineThumbNs + 1;
        std::uint64_t ministrip_id = kMiniStripThumbNs;
        for (int i = 0; i < 50000; ++i) {
            const std::uint64_t t = timeline_id++;
            const std::uint64_t m = ministrip_id++;
            expect("timeline id unique", seen.insert(t).second);
            expect("ministrip id unique", seen.insert(m).second);
            expect("timeline id stays in timeline ns", is_timeline_thumb_id(t));
            expect("ministrip id stays in ministrip ns", is_mini_strip_thumb_id(m));
            expect("timeline id never pool/project", !is_pool_thumb_id(t) && !equals_project_region(t));
            expect("ministrip id never pool/project", !is_pool_thumb_id(m) && !equals_project_region(m));
            expect("ids never hit source-preview sentinel",
                   t != kSourcePreviewId && m != kSourcePreviewId);
        }
    }

    {
        const std::uint64_t t_ok = kTimelineThumbNs + 42;
        const std::uint64_t m_ok = kMiniStripThumbNs + 7;
        const std::uint64_t pool = kPoolNs | 3;
        const std::uint64_t project = kProjectNs | 1;
        expect("timeline gate accepts its own id", is_timeline_thumb_id(t_ok));
        expect("timeline gate rejects ministrip id", !is_timeline_thumb_id(m_ok));
        expect("timeline gate rejects pool id", !is_timeline_thumb_id(pool));
        expect("timeline gate rejects project id", !is_timeline_thumb_id(project));
        expect("ministrip gate accepts its own id", is_mini_strip_thumb_id(m_ok));
        expect("ministrip gate rejects timeline id", !is_mini_strip_thumb_id(t_ok));
        expect("ministrip gate rejects pool id", !is_mini_strip_thumb_id(pool));
        expect("ministrip gate rejects source-preview sentinel",
               !is_mini_strip_thumb_id(kSourcePreviewId));
    }

    {
        expect("pool id passes pool relay", is_pool_thumb_id(kPoolNs | 0));
        expect("pool id w/ non-zero idx passes pool relay", is_pool_thumb_id(kPoolNs | 3));
        expect("project id rejected by pool relay", !is_pool_thumb_id(kProjectNs | 1));
        expect("timeline id rejected by pool relay", !is_pool_thumb_id(kTimelineThumbNs + 1));
        expect("ministrip id rejected by pool relay", !is_pool_thumb_id(kMiniStripThumbNs));
        expect("source-preview sentinel rejected by pool relay",
               !is_pool_thumb_id(kSourcePreviewId));
    }

    std::printf(failures == 0 ? "ALL THUMBS ID-NAMESPACE TESTS PASSED\n"
                              : "%d THUMBS ID-NAMESPACE TEST(S) FAILED\n",
                failures);
    return failures == 0 ? 0 : 1;
}
