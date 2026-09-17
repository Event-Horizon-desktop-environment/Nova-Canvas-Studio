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

}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qRegisterMetaType<uint64_t>("uint64_t");

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
    w.set_frames_per_pixel(1.0);
    w.set_sequence(&seq);
    w.show();

    int failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        if (!ok) {
            failures += 1;
            std::printf("FAIL: %s\n", what);
        }
    };

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

    const double expect_left_x = dxl + static_cast<double>(clip.tl_in) / fpp;
    expect("wave left edge scene-x sits exactly on tl_in grid",
           std::abs(wp.x() - expect_left_x) < 1e-6);

    const double expect_body_w = static_cast<double>(clip.tl_out - clip.tl_in) / fpp;
    const double drawn_w = wave->pixmap().width();
    expect("wave pixmap width == clip body width (no width-4)",
           std::abs(drawn_w - expect_body_w) < 1e-6);
    expect("wave scene rect width == clip body width",
           std::abs(wave->sceneBoundingRect().width() - expect_body_w) < 1e-6);

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
