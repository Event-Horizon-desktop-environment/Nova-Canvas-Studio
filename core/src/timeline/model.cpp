#include "canvas/core/timeline/model.hpp"

#include <algorithm>

namespace canvas::core {

const Clip* Track::clip_at(const int64_t pos) const noexcept {
    for (const auto& clip : clips)
        if (pos >= clip.tl_in && pos < clip.tl_out) return &clip;
    return nullptr;
}

const Clip* Track::clip_with_id(const ClipId id) const noexcept {
    for (const auto& clip : clips)
        if (clip.id == id) return &clip;
    return nullptr;
}

void Track::insert_sorted(Clip clip) {
    auto pos = std::lower_bound(clips.begin(), clips.end(), clip.tl_in,
                                [](const Clip& c, int64_t v) { return c.tl_in < v; });
    clips.insert(pos, std::move(clip));
}

int64_t Track::end_frame() const noexcept {
    int64_t end = 0;
    for (const auto& clip : clips) end = std::max(end, clip.tl_out);
    return end;
}

int64_t Sequence::duration_frames() const noexcept {
    int64_t dur = 0;
    for (const auto& t : video_tracks)
        for (const auto& c : t.clips) dur = std::max(dur, c.tl_out);
    for (const auto& t : audio_tracks)
        for (const auto& c : t.clips) dur = std::max(dur, c.tl_out);
    return dur;
}

Track* Sequence::track(const Track::Kind kind, const std::size_t index) noexcept {
    if (kind == Track::Kind::Video)
        return index < video_tracks.size() ? &video_tracks[index] : nullptr;
    return index < audio_tracks.size() ? &audio_tracks[index] : nullptr;
}

const Track* Sequence::track(const Track::Kind kind, const std::size_t index) const noexcept {
    return const_cast<Sequence*>(this)->track(kind, index);
}

std::size_t Sequence::track_count(const Track::Kind kind) const noexcept {
    return kind == Track::Kind::Video ? video_tracks.size() : audio_tracks.size();
}

bool Sequence::has_bookmark(const int64_t frame) const noexcept {
    for (const auto& b : bookmarks)
        if (b.frame == frame) return true;
    return false;
}

uint64_t Sequence::toggle_bookmark(const int64_t frame, const std::string& label) {
    for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it) {
        if (it->frame == frame) {
            bookmarks.erase(it);
            return 0;
        }
    }
    const uint64_t id = next_bookmark_id++;
    bookmarks.push_back(Bookmark{frame, label, id});
    std::sort(bookmarks.begin(), bookmarks.end(),
              [](const Bookmark& a, const Bookmark& b) { return a.frame < b.frame; });
    return id;
}

bool Sequence::remove_bookmark(const uint64_t id) {
    for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it) {
        if (it->id == id) {
            bookmarks.erase(it);
            return true;
        }
    }
    return false;
}

void Sequence::remove_bookmark_at(const int64_t frame) {
    bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                   [frame](const Bookmark& b) { return b.frame == frame; }),
                    bookmarks.end());
}

}
