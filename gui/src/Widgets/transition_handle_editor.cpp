#include "Widgets/transition_handle_editor.hpp"

namespace canvas::gui {
namespace transition_editor {

int64_t Editor::max_duration(const CutTarget& t) {
    if (!t.valid()) return kMinTransitionFrames;
    if (t.is_cut()) {
        const int64_t maxd = std::min<int64_t>(t.a->duration(), t.b->duration());
        return std::max<int64_t>(kMinTransitionFrames, maxd);
    }
    return std::max<int64_t>(kMinTransitionFrames, t.a->duration());
}

bool Editor::open(const CutTarget& target, int64_t seeded) {
    // A move over the already-open identical target is a no-op, so the widget
    // skips tearing down and rebuilding its painted handle every pointer move.
    const bool unchanged =
        visible_ && t_.cut_frame == target.cut_frame &&
        t_.track_index == target.track_index && t_.a == target.a &&
        t_.b == target.b && t_.edge == target.edge;
    if (unchanged) return false;

    t_ = target;
    int64_t init = seeded < kMinTransitionFrames ? 6 : seeded;
    init = std::clamp(init, kMinTransitionFrames, max_duration(t_));
    dur_ = init;
    // For a single-clip Start (IN) edge the overlay extends rightward from the
    // boundary; an End (OUT) edge extends leftward; a cut centres it across the
    // two clips with the same one-frame-off symmetry as before.
    if (t_.edge == Edge::Start) {
        left_ = t_.cut_frame;
        right_ = t_.cut_frame + init;
    } else if (t_.edge == Edge::End) {
        left_ = t_.cut_frame - init;
        right_ = t_.cut_frame;
    } else {
        left_ = t_.cut_frame - init / 2;
        right_ = t_.cut_frame + (init - init / 2);
    }
    visible_ = true;
    dragging_ = false;
    drag_edge_ = kDragEdgeNone;
    anchor_ = 0;
    snap_ = false;
    return true;
}

void Editor::close() {
    t_ = CutTarget{};
    visible_ = false;
    dragging_ = false;
    drag_edge_ = kDragEdgeNone;
    snap_ = false;
    anchor_ = 0;
    dur_ = 0;
    left_ = 0;
    right_ = 0;
}

void Editor::begin_drag(int edge, bool snap) {
    dragging_ = true;
    drag_edge_ = edge;
    // The opposite edge stays fixed while the grabbed edge moves.
    anchor_ = edge == kDragEdgeLeft ? right_ : left_;
    snap_ = snap;
}

void Editor::move_to(int64_t pointer_frame) {
    const int64_t maxd = max_duration(t_);
    const int64_t rawsize = pointer_frame > anchor_ ? pointer_frame - anchor_
                                                    : anchor_ - pointer_frame;
    int64_t dur = std::clamp(rawsize, kMinTransitionFrames, maxd);
    if (snap_) {
        // Bubble drags snap to the favourite presets so the live label settles
        // on a clean duration instead of a free-form frame count.
        int64_t best = dur;
        for (const int64_t preset : kFavoritePresets) {
            if (preset < kMinTransitionFrames || preset > maxd) continue;
            if (std::llabs(preset - dur) < std::llabs(best - dur)) best = preset;
        }
        dur = best;
    }
    dur_ = dur;
    if (drag_edge_ == kDragEdgeLeft) {
        const int64_t right = anchor_;
        left_ = right - dur;
        right_ = right;
    } else {
        const int64_t left = anchor_;
        left_ = left;
        right_ = left + dur;
    }
}

void Editor::end_drag() {
    dragging_ = false;
    drag_edge_ = kDragEdgeNone;
    snap_ = false;
}

bool Editor::has_transition() const {
    return t_.is_cut()
               ? (t_.a->has_transition_out() || t_.b->has_transition_in())
               : (t_.edge == Edge::Start ? t_.a->has_transition_in()
                                         : t_.a->has_transition_out());
}

int64_t Editor::stored_duration() const {
    return t_.is_cut()
               ? std::max(t_.a->transition_out_duration,
                          t_.b->transition_in_duration)
               : (t_.edge == Edge::Start ? t_.a->transition_in_duration
                                         : t_.a->transition_out_duration);
}

}  // namespace transition_editor
}  // namespace canvas::gui