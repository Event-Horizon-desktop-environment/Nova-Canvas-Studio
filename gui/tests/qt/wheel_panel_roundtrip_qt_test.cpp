#include "features/color/color_widgets.hpp"

#include "canvas/core/grade_graph/graph.hpp"
#include "canvas/core/grade_graph/serialize.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace canvas::gui;
namespace cs = canvas::core::colorsci;
namespace gg = canvas::core::grade_graph;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

bool near(float a, float b) {
    return std::fabs(a - b) < 1e-3f;
}

bool lgg_near(const cs::LGG& a, const cs::LGG& b) {
    return near(a.lift_master, b.lift_master) && near(a.gamma_master, b.gamma_master) &&
           near(a.gain_master, b.gain_master) && near(a.lift_r, b.lift_r) &&
           near(a.lift_g, b.lift_g) && near(a.lift_b, b.lift_b) &&
           near(a.gamma_r, b.gamma_r) && near(a.gamma_g, b.gamma_g) &&
           near(a.gamma_b, b.gamma_b) && near(a.gain_r, b.gain_r) &&
           near(a.gain_g, b.gain_g) && near(a.gain_b, b.gain_b);
}

void drag_to(QWidget* w, const QPointF& press, const QPointF& drop) {
    QMouseEvent press_ev(QEvent::MouseButtonPress, press, press, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press_ev);
    QMouseEvent move_ev(QEvent::MouseMove, drop, drop, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move_ev);
    QMouseEvent release_ev(QEvent::MouseButtonRelease, drop, drop, Qt::LeftButton,
                           Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release_ev);
}

gg::GradeGraph make_lgg_graph(const cs::WheelPanelState& state) {
    gg::GradeGraph g;
    const int corr = g.add_node(gg::NodeKind::kCorrector);
    g.node(corr).correct_mode = gg::CorrectMode::kLgg;
    g.node(corr).lgg = state.lgg;
    const int out = g.add_node(gg::NodeKind::kOutput);
    const int edge = g.add_rgb_edge(corr, out);
    if (edge < 0) g.clear();
    return g;
}

bool corrector_lgg(const gg::GradeGraph& g, cs::LGG& out) {
    for (std::size_t n = 0; n < g.num_nodes(); ++n) {
        if (g.node(n).kind != gg::NodeKind::kCorrector) continue;
        if (!g.node(n).lgg) continue;
        out = *g.node(n).lgg;
        return true;
    }
    return false;
}

}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    canvas::core::Sequence seq;
    seq.audio_tracks.clear();
    seq.video_tracks.resize(1);
    canvas::core::Track& vt = seq.video_tracks[0];
    vt.name = "V1";
    vt.kind = canvas::core::Track::Kind::Video;
    canvas::core::Clip clip;
    clip.id = 1;
    clip.tl_in = 0;
    clip.tl_out = 100;
    clip.src_in = 0;
    vt.clips.push_back(clip);

    ColorWheelsPanel panel;
    panel.resize(1024, 460);
    panel.show();
    QApplication::processEvents();

    int commits = 0;
    int previews = 0;
    int reset_alls = 0;
    QObject::connect(&panel, &ColorWheelsPanel::params_committed, &panel,
                     [&commits](const cs::WheelPanelState&) { ++commits; });
    QObject::connect(&panel, &ColorWheelsPanel::params_preview, &panel,
                     [&previews] { ++previews; });
    QObject::connect(&panel, &ColorWheelsPanel::reset_all_requested, &panel,
                     [&reset_alls] { ++reset_alls; });

    const auto wheels = panel.findChildren<ColorWheelWidget*>();
    expect(static_cast<int>(wheels.size()) == 4, "panel holds four Primaries wheels");
    bool fatal_layout = false;
    if (wheels.size() != 4) {
        fatal_layout = true;
    } else {
        ColorWheelWidget* lift = wheels[0];
        const QPointF disc_c = lift->rect().center();
        const qreal r = (std::min(lift->width(), lift->height()) - 6.0) / 2.0;
        expect(r >= 33.0, "Lift wheel is laid out at a usable size");
        if (r < 33.0) {
            fatal_layout = true;
        } else {
            drag_to(lift, disc_c, disc_c + QPointF(0.8 * r, 0.0));
        }
    }
    if (fatal_layout) {
        std::fprintf(stderr, "FATAL: wheel panel did not lay out\n");
        return 2;
    }

    expect(previews >= 1, "wheel drag previews during the drag");
    expect(commits == 1, "wheel release commits once");
    cs::WheelPanelState st = panel.state();
    expect(st.lgg.lift_r > 0.10f && st.lgg.lift_r <= cs::kLiftHi,
           "Lift wheel right-drag pulls red");
    expect(st.lgg.lift_g < -0.05f && st.lgg.lift_b < -0.05f,
           "Lift wheel right-drag dips green and blue");
    expect(std::fabs(st.lgg.lift_g - st.lgg.lift_b) < 0.05f,
           "green/blue dip stays hue-symmetric");
    expect(near(st.lgg.lift_master, 0.0f), "Lift master stays at identity");

    const auto boxes = panel.findChildren<QDoubleSpinBox*>();
    expect(static_cast<int>(boxes.size()) == 7, "one spin box per shared tone-row slot");
    if (boxes.size() == 7) {
        const int before = commits;
        boxes[static_cast<int>(cs::ToneParam::kTemp)]->setValue(40.0);
        expect(commits == before + 1, "tone edit commits once");
        expect(near(panel.state().temp, 40.0f), "Temp lands in the panel state");
    }

    if (boxes.size() == 7) {
        const int before = commits;
        boxes[static_cast<int>(cs::ToneParam::kBlackOffset)]->setValue(0.35);
        st = panel.state();
        expect(commits > before, "Blk/Offset edit commits");
        expect(near(st.black_offset, 0.35f) && near(st.offset.master, 0.35f),
               "Blk/Offset field and the Offset master stay in lock-step");
    }

    st = panel.state();

    const gg::GradeGraph g = make_lgg_graph(st);
    auto cmd = canvas::core::set_clip_grade(seq, canvas::core::Track::Kind::Video, 0, 1, g);
    expect(cmd != nullptr, "set_clip_grade accepts the committed panel grade");
    if (!cmd) {
        std::fprintf(stderr, "FATAL: set_clip_grade returned null\n");
        return 2;
    }

    const canvas::core::Clip& graded = seq.video_tracks[0].clips[0];
    expect(graded.has_grade(), "clip carries a wired grade after commit");
    cs::LGG out;
    expect(corrector_lgg(graded.grade, out) && lgg_near(out, st.lgg),
           "clip.grade corrector carries exactly the panel LGG");

    canvas::core::UndoStack undo;
    undo.record(std::move(cmd));
    expect(undo.undo(seq), "undo reverts the grade");
    expect(!seq.video_tracks[0].clips[0].has_grade(), "grade cleared after undo");
    expect(undo.redo(seq), "redo re-applies the grade");
    expect(seq.video_tracks[0].clips[0].has_grade(), "grade restored after redo");

    const nlohmann::json j = gg::grade_graph_to_json(g);
    const gg::GradeGraph reloaded = gg::grade_graph_from_json(j);
    cs::LGG out2;
    expect(corrector_lgg(reloaded, out2) && lgg_near(out2, st.lgg),
           "committed grade survives JSON with params intact");

    ColorWheelsPanel fresh;
    fresh.set_state(st);
    const cs::WheelPanelState st2 = fresh.state();
    expect(lgg_near(st2.lgg, st.lgg) && near(st2.temp, st.temp) &&
               near(st2.black_offset, st.black_offset),
           "fresh panel reload keeps the committed params");

    {
        ColorWheelWidget* lift = panel.findChildren<ColorWheelWidget*>()[0];
        const QPointF disc_c = lift->rect().center();
        const qreal r = (std::min(lift->width(), lift->height()) - 6.0) / 2.0;
        const int before = commits;
        drag_to(lift, disc_c + QPointF(0.8 * r, 0.0), disc_c);
        st = panel.state();
        expect(commits == before + 1, "center release commits once");
        expect(near(st.lgg.lift_r, 0.0f) && near(st.lgg.lift_g, 0.0f) &&
                   near(st.lgg.lift_b, 0.0f),
               "center release reverts the wheel to identity");
        expect(near(st.temp, 40.0f), "center release keeps other panel params");
    }

    {
        ColorWheelWidget* lift = panel.findChildren<ColorWheelWidget*>()[0];
        const QPointF disc_c = lift->rect().center();
        const qreal r = (std::min(lift->width(), lift->height()) - 6.0) / 2.0;
        drag_to(lift, disc_c, disc_c + QPointF(0.8 * r, 0.0));
        expect(panel.state().lgg.lift_r > 0.15f, "Lift re-graded before reset");
        QToolButton* reset_btn = nullptr;
        for (QToolButton* b : panel.findChildren<QToolButton*>()) {
            if (b->toolTip() == QLatin1String("Reset wheel")) {
                reset_btn = b;
                break;
            }
        }
        expect(reset_btn != nullptr, "per-wheel reset button exists");
        if (reset_btn) {
            const int before = commits;
            reset_btn->click();
            st = panel.state();
            expect(commits == before + 1, "wheel reset commits once");
            expect(near(st.lgg.lift_r, 0.0f) && near(st.lgg.lift_g, 0.0f) &&
                       near(st.lgg.lift_b, 0.0f) && near(st.lgg.lift_master, 0.0f),
                   "wheel reset reverts the wheel to identity");
        }
    }

    {
        QToolButton* reset_all_btn = nullptr;
        for (QToolButton* b : panel.findChildren<QToolButton*>()) {
            if (b->toolTip() == QLatin1String("Reset all grades")) {
                reset_all_btn = b;
                break;
            }
        }
        expect(reset_all_btn != nullptr, "reset-all button exists");
        if (reset_all_btn) {
            const int before = commits;
            const int before_resets = reset_alls;
            reset_all_btn->click();
            st = panel.state();
            expect(reset_alls == before_resets + 1, "reset-all request reaches the page");
            expect(commits == before, "reset-all does not double-commit through the panel");
            expect(lgg_near(st.lgg, cs::LGG{}), "reset-all reverts every wheel to identity");
        }
    }

    std::printf(failures == 0 ? "ALL QT TESTS PASSED\n" : "%d QT TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
