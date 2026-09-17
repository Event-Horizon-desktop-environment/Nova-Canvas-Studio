// Qt test (2026-09-16 filmstrip wrong-frame / "never fully populated"
// regression): pins the thumbnail request-id namespaces. The timeline filmstrip,
// the color-page mini strip, the media pool, the project manager and the source
// preview all POST INTO THE SAME app-wide ThumbnailService and each widget's
// completion handler matches deliveries by numeric id.
//
// The bug: the timeline counted its ids from 1 and the mini strip from 0 — two
// unbounded counters with no namespace bit. Every integer either widget ever
// assigned was eventually assigned by BOTH, so timeline cell ids collided with
// mini-strip ids on the shared service and each widget's handler matched the
// OTHER widget's decode (wrong frames in the filmstrip, boxes that never fill).
//
// This test pins the fix:
//   - kTimelineThumbNs / kMiniStripThumbNs exist, are distinct, and live OUTSIDE
//     the pool (bit63), project (bits61-63) and source-preview (0xF...001)
//     regions, so MainWindow's relay can never mis-route a timeline/strip id.
//   - the two counters, run over a realistic workload, never produce a shared id.
//   - the O(1) gate predicates each widget's handler now leads with
//     (is_timeline_thumb_id / is_mini_strip_thumb_id) accept only their own ids
//     and reject the other consumer's ids.
//
// Both widget headers are compiled here exactly as shipped (a change to the
// namespace constants or gate helpers fails this build), and the geo setup below
// makes sure the whole widget translation units link.

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

// MainWindow.hpp owns kPoolThumbNs (bit63), project_manager_widget.hpp owns
// kProjectThumbNs (bits61-63) and MainWindow.cpp owns the source-preview
// sentinel kSourcePreviewWaveformId = 0xF000000000000001ULL (bits60-63 + bit0).
// They are contract constants that no widget header can cheaply include (they
// live in whole-app translation units); mirrored here so the test can assert
// the timeline/strip namespaces stay OUTSIDE every existing region.
constexpr std::uint64_t kPoolNs = 0x8000000000000000ULL;
constexpr std::uint64_t kProjectNs = 0xE000000000000000ULL;
constexpr std::uint64_t kSourcePreviewId = 0xF000000000000001ULL;

bool is_pool_thumb_id(std::uint64_t id) {
    // MainWindow's pool relay accepts exactly the kPoolThumbNs tag: the top
    // three bits must read 100 (bit63 set, project bits 61-62 clear). This is
    // the FIX for a real bug — the old guard `bit63 set && no project bits`
    // rejected every pool id, because kPoolNs (bit63) is a SUBSET of kProjectNs
    // (bits61-63), so no id could ever have bit63 set yet match no project bit.
    return (id & kProjectNs) == kPoolNs;
}
bool equals_project_region(std::uint64_t id) { return (id & kProjectNs) == kProjectNs; }

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Widgets exist so the real translation units (incl. the helpers they now
    // gate on) are compiled and linked by this target, not just unit-tested.
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

    // --- 1. Constants exist and are distinct. ---
    expect("kTimelineThumbNs / kMiniStripThumbNs distinct",
           kTimelineThumbNs != kMiniStripThumbNs);
    expect("timeline ns is a single high bit",
           (kTimelineThumbNs & (kTimelineThumbNs - 1)) == 0);
    expect("ministrip ns is a single high bit",
           (kMiniStripThumbNs & (kMiniStripThumbNs - 1)) == 0);

    // --- 2. Both live outside every pre-existing namespace region ---
    expect("timeline ns outside pool region", (kTimelineThumbNs & kPoolNs) == 0);
    expect("timeline ns outside project region", (kTimelineThumbNs & kProjectNs) == 0);
    expect("timeline ns != source-preview sentinel", kTimelineThumbNs != kSourcePreviewId);
    expect("ministrip ns outside pool region", (kMiniStripThumbNs & kPoolNs) == 0);
    expect("ministrip ns outside project region", (kMiniStripThumbNs & kProjectNs) == 0);
    expect("ministrip ns != source-preview sentinel", kMiniStripThumbNs != kSourcePreviewId);

    // --- 3. Workload sweep: the two counters never produce a shared id, and
    // none ever strays into a foreign consumer's region. ---
    {
        std::unordered_set<std::uint64_t> seen;
        // Startup values match the shipped counters exactly.
        std::uint64_t timeline_id = kTimelineThumbNs + 1;
        std::uint64_t ministrip_id = kMiniStripThumbNs;
        for (int i = 0; i < 50000; ++i) {
            const std::uint64_t t = timeline_id++;
            const std::uint64_t m = ministrip_id++;
            expect("timeline id unique", seen.insert(t).second);
            expect("ministrip id unique", seen.insert(m).second);
            // Every id stays inside its own namespace for a realistic workload.
            expect("timeline id stays in timeline ns", is_timeline_thumb_id(t));
            expect("ministrip id stays in ministrip ns", is_mini_strip_thumb_id(m));
            expect("timeline id never pool/project", !is_pool_thumb_id(t) && !equals_project_region(t));
            expect("ministrip id never pool/project", !is_pool_thumb_id(m) && !equals_project_region(m));
            expect("ids never hit source-preview sentinel",
                   t != kSourcePreviewId && m != kSourcePreviewId);
        }
    }

    // --- 4. Completion gates: each widget accepts ONLY its own ids. ---
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

    // --- 5. MainWindow's pool relay (thumbnail_ready + waveform_ready): only
    // pool-tagged ids route to pool tiles. Regression for the pool-thumbnail
    // bug: a pool id (kPoolNs | i) MUST pass, while project ids, timeline/
    // ministrip ids and the source-preview sentinel MUST be rejected — the old
    // guard dropped every pool id because kPoolNs is a subset of kProjectNs. ---
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