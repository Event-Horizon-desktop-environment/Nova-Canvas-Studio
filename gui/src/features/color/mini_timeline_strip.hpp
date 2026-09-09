#pragma once

// Mini-timeline strip (design spec §Layout-5): a compact cosmetic overview of
// the sequence shown above the Color workspace. Reads the project's sequence
// and paints a filmstrip of the clips on the topmost video track plus a flat
// audio band, with index chips, names and a live playhead. Clicking a chip
// seeks the playhead to that clip (Milestone 0 UX scaffold — no grade data).

#include <QWidget>

#include <cstdint>

#include "canvas/core/timeline/model.hpp"

namespace canvas::core {
struct Sequence;
}

namespace canvas::gui {

class MiniTimelineStrip : public QWidget {
    Q_OBJECT
public:
    explicit MiniTimelineStrip(QWidget* parent = nullptr);

    void set_sequence(const canvas::core::Sequence* sequence);
    void set_playhead(int64_t frame);

    [[nodiscard]] const canvas::core::Sequence* sequence() const { return sequence_; }

signals:
    void clip_activated(canvas::core::ClipId id, int64_t timeline_frame);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int64_t total_frames() const;
    const canvas::core::Sequence* sequence_ = nullptr;
    int64_t playhead_ = 0;
};

}  // namespace canvas::gui