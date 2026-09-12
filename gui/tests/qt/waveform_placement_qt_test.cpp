// Qt widget test (2026-09-08 wrong-cut regression): pins the drawn waveform's
// placement to the SAME geometry law the razor cuts on. The bug: the wave pixmap
// was positioned at (cx+2, cy+2) and resized to body_width-4 while frame_at_x()
// (and therefore the blade) uses scene_x = kSceneMargin + kTrackHeaderWidth +
// frame/fpp with NO inset. A transient aimed at a clip's edge landed N frames off
// ("blade first cut wrong" returns). This test feeds a synthetic waveform through
// the real on_waveform_ready placement path and asserts:
//   left scene-x  == kSceneMargin + kTrackHeaderWidth + tl_in/fpp
//   pixmap width  == clip body width (duration/fpp)
//   mapping back: left edge -> tl_in, right edge -> tl_out (to within 0.01 f)
//
// Qt-linked on purpose (drives QGraphicsScene item placement exactly as shipped).

#include "Widgets/timeline_widget.hpp"

#include "canvas/core/timeline/model.hpp"

#include <QApplication>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImage>
#include <QMetaObject>
#include <QPointF>
#include <QtTest/QtTest>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace canvas::gui;

namespace {

// Locate the audio clip's waveform pixmap item: the one QGraphicsPixmapItem with
// a NON-NULL pixmap at z==1 (the wave). Video filmstrip cells are z<1 and null
// until a thumbnail lands (this test has no video media), so the wave is unique.
QGraphicsPixmapItem* waveform_item(TimelineWidget& w) {
    for (QGraphicsItem* it : w.scene()->items()) {
        auto* pm = dynamic_cast<QGraphicsPixmapItem*>(it);
        if (!pm) continue;
        if (std::abs(pm->zValue() - 1.0) > 1e-6) continue;
        if (pm->pixmap().isNull()) continue;
        return pm;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qRegisterMetaType<uint64_t>("uint64_t");

    // One audio track, one clip far enough inside the timeline that any
    // left/bottom offset would show up as a frame count on BOTH edges.
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
    clip.src_out = 100;
    clip.volume_db = 0.0f;
    at.clips.push_back(clip);

    TimelineWidget w;
    w.resize(1200, 600);
    w.set_frames_per_pixel(1.0);  // pinned so frame == pixel 1:1
    w.set_sequence(&seq);
    w.show();

    int failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        if (!ok) {
            failures += 1;
            std::printf("FAIL: %s\n", what);
        }
    };

    // Deliver a synthetic waveform through the real placement path. No thumbnail
    // service is set, so cells[0].request_id stays 0 and the slot accepts id 0.
    QImage img(64, 16, QImage::Format_ARGB32);
    img.fill(Qt::white);
    const bool invoked = QMetaObject::invokeMethod(
        &w, "on_waveform_ready", Qt::DirectConnection,
        Q_ARG(uint64_t, 0), Q_ARG(QImage, img));
    expect("on_waveform_ready slot invokable", invoked);

    QGraphicsPixmapItem* wave = waveform_item(w);
    expect("waveform pixmap item exists after delivery", wave != nullptr);
    if (!wave) {
        std::printf("%d QT TEST(S) FAILED\n", failures);
        return failures == 0 ? 0 : 1;
    }

    const double fpp = w.frames_per_pixel();
    const double dxl = static_cast<double>(kSceneMargin + TimelineWidget::kTrackHeaderWidth);
    const QPointF wp = wave->scenePos();
    const QPointF wl = wave->sceneBoundingRect().topLeft();

    // The law frame_at_x() (and the blade) applies: scene_x = dxl + frame/fpp.
    // The drawn wave's left edge must sit exactly at tl_in's scene-x.
    const double expect_left_x = dxl + static_cast<double>(clip.tl_in) / fpp;
    expect("wave left edge scene-x sits exactly on tl_in grid",
           std::abs(wp.x() - expect_left_x) < 1e-6);

    // The pixmap must occupy the FULL clip body width (the REQ generated it at
    // that width; a width-4 rescale or a x+2 inset both shift drawn frames).
    const double expect_body_w = static_cast<double>(clip.tl_out - clip.tl_in) / fpp;
    const double drawn_w = wave->pixmap().width();
    expect("wave pixmap width == clip body width (no width-4)",
           std::abs(drawn_w - expect_body_w) < 1e-6);
    expect("wave scene rect width == clip body width",
           std::abs(wave->sceneBoundingRect().width() - expect_body_w) < 1e-6);

    // Frame mapping the PLACE audit uses: left/right edges map back to the clip
    // tl window within a sub-frame tolerance.
    const double drawn_left_frame = (wl.x() - dxl) * fpp;
    const double drawn_right_frame = (wl.x() + wave->sceneBoundingRect().width() - dxl) * fpp;
    std::printf("wave: scene_x=%g body_w=%g drawn_frames=[%g,%g]\n",
                wp.x(), drawn_w, drawn_left_frame, drawn_right_frame);
    expect("drawn left edge == tl_in", std::abs(drawn_left_frame - clip.tl_in) < 0.01);
    expect("drawn right edge == tl_out", std::abs(drawn_right_frame - clip.tl_out) < 0.01);

    std::printf(failures == 0 ? "ALL WAVE PLACEMENT TESTS PASSED\n"
                              : "%d WAVE PLACEMENT TEST(S) FAILED\n",
                failures);
    return failures == 0 ? 0 : 1;
}