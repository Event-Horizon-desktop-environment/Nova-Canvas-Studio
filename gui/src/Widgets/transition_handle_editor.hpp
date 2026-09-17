#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "canvas/core/timeline/model.hpp"

namespace canvas::gui {
namespace transition_editor {

enum class Edge { Cut, Start, End };

struct CutTarget {
    const canvas::core::Clip* a = nullptr;
    const canvas::core::Clip* b = nullptr;
    canvas::core::Track::Kind kind = canvas::core::Track::Kind::Video;
    int track_index = 0;
    int64_t cut_frame = 0;
    Edge edge = Edge::Cut;

    [[nodiscard]] bool valid() const noexcept { return a != nullptr; }
    [[nodiscard]] bool is_cut() const noexcept { return b != nullptr; }
};

constexpr int64_t kMinTransitionFrames = 1;

constexpr int kDragEdgeNone = -1;
constexpr int kDragEdgeLeft = 0;
constexpr int kDragEdgeRight = 1;

constexpr int64_t kFavoritePresets[] = {14, 30, 60, 120};

class Editor {
public:
    static int64_t max_duration(const CutTarget& t);

    bool open(const CutTarget& target, int64_t seeded);

    void close();

    void begin_drag(int edge, bool snap);
    void move_to(int64_t pointer_frame);
    void end_drag();

    [[nodiscard]] bool has_transition() const;
    [[nodiscard]] int64_t stored_duration() const;

    [[nodiscard]] bool visible() const { return visible_; }
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool snap() const { return snap_; }
    [[nodiscard]] int drag_edge() const { return drag_edge_; }
    [[nodiscard]] int64_t duration() const { return dur_; }
    [[nodiscard]] int64_t left() const { return left_; }
    [[nodiscard]] int64_t right() const { return right_; }
    [[nodiscard]] const CutTarget& target() const { return t_; }

private:
    CutTarget t_;
    bool visible_ = false;
    bool dragging_ = false;
    int drag_edge_ = kDragEdgeNone;
    bool snap_ = false;
    int64_t anchor_ = 0;
    int64_t dur_ = 0;
    int64_t left_ = 0;
    int64_t right_ = 0;
};

}

}