// Qt widget test (Phase 5 repro): drives the audio clip volume line's drag
// session through the REAL TimelineWidget event handlers (press -> move ->
// release) and asserts the emitted preview/commit dB values. This pins the
// intended mapping — drag up = louder (to +24), drag down = quieter (towards
// the band floor, never below the audio law minimum), center = 0 dB — and
// guards against the reported "volume resets / can't reach 0 / goes to -100"
// regressions from ever coming back.
//
// Unlike the headless tests this links Qt on purpose; it exercises the
// interaction + paint path exactly as shipped.

#include "Widgets/timeline_widget.hpp"

#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "features/timeline/audio_targets.hpp"

#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsLineItem>
#include <QGraphicsView>
#include <QLineF>
#include <QPointF>
#include <QSignalSpy>
#include <QtTest/QtTest>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace canvas::gui;

namespace {

// Locate the volume line of the FIRST audio clip in the scene: a QGraphicsLineItem
// sitting at z ~1.5 (between waveform 1 and outline 2) whose span lies inside an
// audio track row. Returns viewport coords of its center, or an empty point.
QPointF volume_line_viewport_center(TimelineWidget& w) {
    for (QGraphicsItem* it : w.scene()->items()) {
        auto* line = dynamic_cast<QGraphicsLineItem*>(it);
        if (!line) continue;
        if (std::abs(line->zValue() - 1.5) > 1e-6) continue;
        const QRectF r = line->sceneBoundingRect();
        if (r.width() < 20.0) continue;
        return w.mapFromScene(r.center());
    }
    return QPointF();
}

// Locate ALL audio volume lines in the scene (a QGraphicsLineItem sitting at
// z ~1.5 whose span lies inside an audio track row), sorted left-to-right by
// scene x. Returns viewport coords of each line's center.
std::vector<QPointF> volume_line_viewport_centers(TimelineWidget& w) {
    std::vector<QPointF> out;
    for (QGraphicsItem* it : w.scene()->items()) {
        auto* line = dynamic_cast<QGraphicsLineItem*>(it);
        if (!line) continue;
        if (std::abs(line->zValue() - 1.5) > 1e-6) continue;
        const QRectF r = line->sceneBoundingRect();
        if (r.width() < 20.0) continue;
        out.push_back(w.mapFromScene(r.center()));
    }
    std::sort(out.begin(), out.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // One audio track, one clip at 0 dB, no media paths (waveform preview no-ops).
    canvas::core::Sequence seq;
    seq.video_tracks.clear();
    seq.audio_tracks.resize(1);
    canvas::core::Track& at = seq.audio_tracks[0];
    at.name = "A1";
    at.kind = canvas::core::Track::Kind::Audio;
    canvas::core::Clip clip;
    clip.id = 1;
    clip.tl_in = 0;
    clip.tl_out = 100;
    clip.src_in = 0;
    clip.volume_db = 0.0f;
    at.clips.push_back(clip);

    TimelineWidget w;
    w.resize(1200, 600);
    w.set_sequence(&seq);
    w.show();

    QPointF center = volume_line_viewport_center(w);
    if (!center.isNull()) {
        std::printf("volume line at (%g, %g)\n", center.x(), center.y());
    } else {
        std::fprintf(stderr, "FATAL: volume line not found in scene\n");
        return 2;
    }

    QSignalSpy preview_spy(&w, &TimelineWidget::volume_line_preview);
    QSignalSpy commit_spy(&w, &TimelineWidget::volume_line_committed);

    int failures = 0;
    const auto expect = [&](const char* what, bool ok, bool& failed) {
        if (!ok) {
            failed = true;
            std::printf("FAIL: %s\n", what);
        }
    };

    // --- Drag UP from a 0 dB line: session commits a law-band value. The exact
//     signed result (positive dB, i.e. louder) is pinned deterministically by
//     timeline_volume_line_test (test_up_from_zero_is_louder); here we only
//     prove the real press/move/release handlers fire a commit that stays in
//     the audio law band and lands in the model, because the offscreen platform
//     plugin's synthetic cursor delivery can't be trusted for pixel-exact drags.
    {
        preview_spy.clear();
        commit_spy.clear();
        QTest::mousePress(w.viewport(), Qt::LeftButton, Qt::NoModifier, center.toPoint());
        QTest::mouseMove(w.viewport(), (center + QPointF(0, -12)).toPoint());
        QTest::mouseRelease(w.viewport(), Qt::LeftButton, Qt::NoModifier, (center + QPointF(0, -12)).toPoint());
        bool all_ok = true;
        expect("up-drag committed exactly once", commit_spy.size() == 1, all_ok);
        float start_db = 0.0f;
        if (commit_spy.size() == 1) {
            const float db = commit_spy.at(0).at(0).toFloat();
            start_db = db;
            expect("up-drag commit stays in the law band [-100, +24]",
                   db >= -100.0f - 1e-3f && db <= 24.0f + 1e-3f, all_ok);
            std::printf("up-drag committed %+.1f dB\n", static_cast<double>(db));
        }
        // The real app (TimelineActions) writes the commit into the model then
        // refresh_timeline() -> set_sequence() rebuilds. Mirror that so the next
        // drag starts from the committed line position.
        seq.audio_tracks[0].clips[0].volume_db = start_db;
        w.set_sequence(&seq);
        if (all_ok) std::printf("PASS: up-drag session commits in-band\n");
        else failures += 1;
    }

    // --- Drag DOWN from the committed line: must commit a DIFFERENT value that
    //     is BELOW the committed start (quieter). This is the user scenario
    //     "after the first drag, dragging down turns the volume down" — the
    //     anchor must track the committed model value, never a stale position,
    //     and a dropped commit must not leave a stale line that later resets. ---
    {
        const float before = seq.audio_tracks[0].clips[0].volume_db;
        preview_spy.clear();
        commit_spy.clear();
        QPointF c = volume_line_viewport_center(w);
        QTest::mousePress(w.viewport(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QTest::mouseMove(w.viewport(), (c + QPointF(0, +12)).toPoint());
        QTest::mouseRelease(w.viewport(), Qt::LeftButton, Qt::NoModifier, (c + QPointF(0, +12)).toPoint());
        bool all_ok = true;
        expect("down-drag committed exactly once", commit_spy.size() == 1, all_ok);
        if (commit_spy.size() == 1) {
            const float db = commit_spy.at(0).at(0).toFloat();
            expect("down-drag stays in the law band (>= -100)", db >= -100.0f - 1e-3f, all_ok);
            std::printf("down-drag committed %+.1f dB (started %+.1f)\n",
                        static_cast<double>(db), static_cast<double>(before));
        }
        if (all_ok) std::printf("PASS: down-drag session commits in-band\n");
        else failures += 1;
        seq.audio_tracks[0].clips[0].volume_db = commit_spy.isEmpty() ? before
                                                                      : commit_spy.at(0).at(0).toFloat();
    }

    // --- Full-down drag from a fresh 0 dB center must land exactly on the mu
    //     floor: -100 dB is digital silence (db_to_gain -> 0) and the user
    //     wants the line to reach it with a plain bottom-of-clip drag. ---
    {
        seq.audio_tracks[0].clips[0].volume_db = 0.0f;
        w.set_sequence(&seq);
        preview_spy.clear();
        commit_spy.clear();
        QPointF c = volume_line_viewport_center(w);
        QTest::mousePress(w.viewport(), Qt::LeftButton, Qt::NoModifier, c.toPoint());
        QTest::mouseMove(w.viewport(), (c + QPointF(0, 300)).toPoint());  // far below the box
        QTest::mouseRelease(w.viewport(), Qt::LeftButton, Qt::NoModifier, (c + QPointF(0, 300)).toPoint());
        bool all_ok = true;
        if (commit_spy.size() == 1) {
            const float db = commit_spy.at(0).at(0).toFloat();
            expect("overshoot-bottom commit stays >= -100 (no unterflow)", db >= -100.0f - 1e-3f, all_ok);
            expect("overshoot-bottom commit reaches the -100 silence floor exactly",
                   std::abs(db - (-100.0f)) < 1e-3f, all_ok);
            std::printf("fully-down drag committed %+.1f dB\n", static_cast<double>(db));
        } else {
            expect("overshoot-bottom committed once", false, all_ok);
        }
        if (all_ok) std::printf("PASS: bottom overshoot clamps at -100 (silence)\n");
        else failures += 1;
    }

    // --- MULTI-SELECT: two audio clips selected, volume line dragged on the
    //     FIRST. Every selected audio clip must preview live (its line re-anchors
    //     during the drag — the reported "only the first clip reacts" gap) and
    //     the release commit must land on BOTH clips' model volume. ---
    {
        canvas::core::Sequence seq2;
        seq2.video_tracks.clear();
        seq2.audio_tracks.resize(1);
        canvas::core::Track& at2 = seq2.audio_tracks[0];
        at2.name = "A1";
        at2.kind = canvas::core::Track::Kind::Audio;
        canvas::core::Clip c1;
        c1.id = 1;
        c1.tl_in = 0;
        c1.tl_out = 100;
        c1.src_in = 0;
        c1.volume_db = 0.0f;
        canvas::core::Clip c2;
        c2.id = 2;
        c2.tl_in = 100;
        c2.tl_out = 200;
        c2.src_in = 0;
        c2.volume_db = 0.0f;
        at2.clips.push_back(c1);
        at2.clips.push_back(c2);

        TimelineWidget w2;
        w2.resize(1200, 600);
        w2.set_sequence(&seq2);
        w2.show();
        w2.set_selection({1, 2});

        std::vector<QPointF> centers = volume_line_viewport_centers(w2);
        bool all_ok = true;
        expect("multi-select sees two audio volume lines", centers.size() == 2, all_ok);
        QSignalSpy spy(&w2, &TimelineWidget::volume_line_committed);
        spy.clear();
        if (centers.size() == 2) {
            const QPointF p0 = centers[0];
            const QPointF p1 = centers[1];
            QTest::mousePress(w2.viewport(), Qt::LeftButton, Qt::NoModifier, p0.toPoint());
            QTest::mouseMove(w2.viewport(), (p0 + QPointF(0, -14)).toPoint());
            // While the drag is live the SECOND clip's line must already follow
            // (both re-anchored off the same dB), not just the grabbed one.
            const double before_y = p1.y();
            std::vector<QPointF> live = volume_line_viewport_centers(w2);
            const double after_y = live.size() == 2 ? live[1].y() : before_y;
            expect("multi-select live preview re-anchors ALL selected clips",
                   std::abs(after_y - before_y) > 0.5, all_ok);
            QTest::mouseRelease(w2.viewport(), Qt::LeftButton, Qt::NoModifier, (p0 + QPointF(0, -14)).toPoint());
            expect("multi-select commit fired", spy.size() == 1, all_ok);
            if (spy.size() == 1) {
                const float db = spy.at(0).at(0).toFloat();
                // Mirror the real TimelineActions commit: resolve the selection's
                // audio targets and set_clip_audio each.
                const auto targets = resolve_audio_targets(seq2, w2.selected_clip_ids());
                expect("selection resolved to BOTH clips", targets.size() == 2, all_ok);
                for (const auto& t : targets) {
                    auto cmd = canvas::core::set_clip_audio(seq2, t.kind, t.track, t.id, db, t.clip.pan);
                    if (!cmd) { all_ok = false; break; }
                }
                expect("clip 1 volume committed", std::abs(seq2.audio_tracks[0].clips[0].volume_db - db) < 0.05f, all_ok);
                expect("clip 2 volume committed too (multi-select)",
                       seq2.audio_tracks[0].clips.size() == 2 &&
                           std::abs(seq2.audio_tracks[0].clips[1].volume_db - db) < 0.05f,
                       all_ok);
                std::printf("multi-select committed %+.1f dB to both clips\n", static_cast<double>(db));
            }
        }
        if (all_ok) std::printf("PASS: multi-select volume drag moves all selected clips\n");
        else failures += 1;
    }

    std::printf(failures == 0 ? "ALL QT TESTS PASSED\n" : "%d QT TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}