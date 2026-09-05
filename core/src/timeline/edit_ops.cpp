#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>

namespace canvas::core {

namespace {

std::vector<Clip> clipped_range(const std::vector<Clip>& clips, const int64_t in,
                                const int64_t out) {
    std::vector<Clip> kept;
    kept.reserve(clips.size());
    for (auto clip : clips) {
        if (clip.tl_out <= in || clip.tl_in >= out) {
            kept.push_back(std::move(clip));
            continue;
        }
        if (clip.tl_in < in) {
            Clip head = clip;
            head.tl_out = in;
            head.src_out = head.src_in + head.duration();
            kept.push_back(std::move(head));
        }
        if (clip.tl_out > out) {
            Clip tail = clip;
            tail.tl_in = out;
            tail.src_in = tail.src_out - tail.duration();
            kept.push_back(std::move(tail));
        }
    }
    return kept;
}

TrackSnapshot snapshot(const Sequence& seq, const Track::Kind kind, const std::size_t index) {
    const Track* t = seq.track(kind, index);
    assert(t);
    return {kind, index, t->clips};
}

void shift_from(std::vector<Clip>& clips, const int64_t from, const int64_t delta) {
    for (auto& c : clips)
        if (c.tl_in >= from) {
            c.tl_in += delta;
            c.tl_out += delta;
        }
}

class SingleTrackEdit {
public:
    SingleTrackEdit(Sequence& s, const Track::Kind k, const std::size_t i, std::string name)
        : seq_(s), kind_(k), index_(i), name_(std::move(name)) {
        before_.push_back(snapshot(seq_, kind_, index_));
    }

    [[nodiscard]] Track* track() const { return seq_.track(kind_, index_); }

    std::unique_ptr<ICommand> finish() {
        std::vector<TrackSnapshot> after{snapshot(seq_, kind_, index_)};
        return std::make_unique<EditCommand>(name_, std::move(before_), std::move(after));
    }

private:
    Sequence& seq_;
    Track::Kind kind_;
    std::size_t index_;
    std::string name_;
    std::vector<TrackSnapshot> before_;
};

struct TrackRef {
    Track::Kind kind;
    std::size_t index;

    bool operator==(const TrackRef& o) const noexcept {
        return kind == o.kind && index == o.index;
    }
};

void collect_track(std::vector<TrackRef>& vec, const Sequence& seq, const TrackRef& ref) {
    if (seq.track_count(ref.kind) > ref.index &&
        std::find(vec.begin(), vec.end(), ref) == vec.end()) {
        vec.push_back(ref);
    }
}

std::optional<TrackRef> find_clip_ref(Sequence& seq, const ClipId id, Track** out_track) {
    for (int ki = 0; ki < 2; ++ki) {
        const Track::Kind k = ki == 0 ? Track::Kind::Video : Track::Kind::Audio;
        for (std::size_t i = 0; i < seq.track_count(k); ++i) {
            Track* t = seq.track(k, i);
            if (t->clip_with_id(id)) {
                if (out_track) *out_track = t;
                return TrackRef{k, i};
            }
        }
    }
    return std::nullopt;
}

std::vector<TrackSnapshot> take_snapshots(const Sequence& seq, const std::vector<TrackRef>& refs) {
    std::vector<TrackSnapshot> out;
    out.reserve(refs.size());
    for (const auto& r : refs) out.push_back(snapshot(seq, r.kind, r.index));
    return out;
}

// Which edge of a clip a transition applies to. OUT (trailing) is the original
// concept (blend into whatever follows); IN (leading) fades the clip in at its
// head with no preceding clip required.
enum class TransitionEdge { Out, In };

// Per-kind transition mapping for a LINKED A/V pair. A transition stored on a
// video clip must also exist on its audio mate in AUDIO form (and vice versa),
// or it is inert: the video renderer ignores AudioFade* and the audio mixers
// ignore CrossDissolve/Fade/Wipe. Mapping the mate into its own domain means one
// edit puts BOTH a visual transition on the video clip AND an audible fade on
// the audio clip (Resolve-style linked-pair behavior).
TransitionType audio_transition_from_video(const TransitionType t) {
    switch (t) {
        case TransitionType::CrossDissolve:
            return TransitionType::AudioFadeConstantPower;  // dissolve = equal-power crossfade
        case TransitionType::DipToBlack:
            return TransitionType::AudioFadeConstantGain;   // dip = dip to silence
        case TransitionType::FadeOut:
            return TransitionType::AudioFadeConstantGain;   // fade out = fade to silence
        case TransitionType::FadeIn:
            return TransitionType::AudioFadeConstantGain;   // fade in = fade up from silence
        default:
            // Wipes etc.: the closest audio analogue is the equal-power crossfade.
            return TransitionType::AudioFadeConstantPower;
    }
}

TransitionType video_transition_from_audio(const TransitionType t, const TransitionEdge edge) {
    switch (t) {
        case TransitionType::AudioFadeConstantPower:
            return TransitionType::CrossDissolve;
        case TransitionType::AudioFadeExponential:
        case TransitionType::AudioFadeConstantGain:
        default:
            return edge == TransitionEdge::In ? TransitionType::FadeIn
                                              : TransitionType::FadeOut;
    }
}

// Shared implementation for set_clip_transition*: writes `type`/`duration` to
// either the IN or OUT edge of the clip (and its linked mate, if any), inside a
// single undoable snapshot pair. Returns nullptr if the clip is not found.
std::unique_ptr<ICommand> set_clip_transition_edge(Sequence& seq, const Track::Kind kind,
                                                   const std::size_t track_index, const ClipId id,
                                                   const TransitionEdge edge, const TransitionType type,
                                                   const int64_t duration) {
    Track* t = seq.track(kind, track_index);
    const Clip* c = t ? t->clip_with_id(id) : nullptr;
    if (!c) return nullptr;
    if (duration < 0) return nullptr;

    std::vector<TrackRef> involved{{kind, track_index}};

    Track* mate_track = nullptr;
    ClipId mate_id = 0;
    if (c->linked_id != 0) {
        if (const auto ref = find_clip_ref(seq, c->linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate_id = c->linked_id;
        }
    }
    // The linked mate lives on the opposite kind of track. Translate the
    // requested type into the DOMAIN the mate actually renders so both sides of
    // the pair get a live transition (video type on the video clip, audio fade
    // on the audio clip). A type that already matches the mate's kind is kept.
    const Track::Kind mate_kind =
        kind == Track::Kind::Video ? Track::Kind::Audio : Track::Kind::Video;
    const bool mate_wants_audio = mate_kind == Track::Kind::Audio;
    TransitionType mate_type = type;
    if (mate_id != 0 && type != TransitionType::None) {
        if (is_audio_transition(type)) {
            if (!mate_wants_audio) mate_type = video_transition_from_audio(type, edge);
        } else {
            if (mate_wants_audio) mate_type = audio_transition_from_video(type);
        }
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    const auto apply = [](Clip& cc, const TransitionEdge e, const TransitionType ty, const int64_t du) {
        if (e == TransitionEdge::In) {
            cc.transition_in = ty;
            cc.transition_in_duration = du;
        } else {
            cc.transition_out = ty;
            cc.transition_out_duration = du;
        }
    };

    for (auto& cc : t->clips)
        if (cc.id == id) { apply(cc, edge, type, duration); break; }
    if (mate_track && mate_id != 0) {
        for (auto& mc : mate_track->clips)
            if (mc.id == mate_id) { apply(mc, edge, mate_type, duration); break; }
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    CANVAS_LOG("transition: set %s kind=%d track=%zu id=%lld type=%d duration=%lld linked=%lld",
           edge == TransitionEdge::In ? "in" : "out",
           static_cast<int>(kind), track_index, (long long)id, static_cast<int>(type),
           (long long)duration, (long long)mate_id);
    return std::make_unique<EditCommand>("set transition", std::move(before), std::move(after));
}

// Logs every clip that disappeared between the `before` and `after` snapshots
// of a delete operation. This is the ground truth of which clips an edit
// actually removed (across all involved tracks, including any linked mates).
void log_deleted_clips(const std::vector<TrackSnapshot>& before,
                       const std::vector<TrackSnapshot>& after) {
    for (const auto& b : before) {
        for (const auto& c : b.clips) {
            bool present_after = false;
            for (const auto& a : after)
                if (a.kind == b.kind && a.index == b.index) {
                    for (const auto& ac : a.clips)
                        if (ac.id == c.id) { present_after = true; break; }
                    break;
                }
            if (!present_after)
                CANVAS_LOG("delete: REMOVED clip id=%lld kind=%d track=%zu tl=[%lld,%lld) linked=%lld",
                       (long long)c.id, (int)b.kind, b.index, (long long)c.tl_in,
                       (long long)c.tl_out, (long long)c.linked_id);
        }
    }
}

}  // namespace

EditCommand::EditCommand(std::string name, std::vector<TrackSnapshot> before,
                         std::vector<TrackSnapshot> after)
    : name_(std::move(name)), before_(std::move(before)), after_(std::move(after)) {}

void EditCommand::apply(Sequence& seq, const std::vector<TrackSnapshot>& state) {
    for (const auto& snap : state) {
        Track* t = seq.track(snap.kind, snap.index);
        assert(t);
        t->clips = snap.clips;
    }
}

void EditCommand::redo(Sequence& seq) { apply(seq, after_); }
void EditCommand::undo(Sequence& seq) { apply(seq, before_); }

void UndoStack::record(std::unique_ptr<ICommand> command) {
    redo_.clear();
    undo_.push_back(std::move(command));
    constexpr std::size_t kMaxDepth = 256;
    if (undo_.size() > kMaxDepth) undo_.erase(undo_.begin());
}

bool UndoStack::undo(Sequence& seq) {
    if (undo_.empty()) return false;
    undo_.back()->undo(seq);
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

bool UndoStack::redo(Sequence& seq) {
    if (redo_.empty()) return false;
    redo_.back()->redo(seq);
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    return true;
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
}

std::unique_ptr<ICommand> place_clip(Sequence& seq, const Track::Kind kind,
                                     const std::size_t track_index, Clip clip,
                                     const Placement mode) {
    if (mode == Placement::PlaceOnTop && kind == Track::Kind::Video) {
        for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
            const Track& t = seq.video_tracks[i];
            bool free = true;
            for (const auto& c : t.clips)
                if (clip.tl_in < c.tl_out && c.tl_in < clip.tl_out) {
                    free = false;
                    break;
                }
            if (free) return place_clip(seq, kind, i, std::move(clip), Placement::Overwrite);
        }
        Track extra;
        extra.kind = Track::Kind::Video;
        extra.name = "V" + std::to_string(seq.video_tracks.size() + 1);
        const std::size_t new_index = seq.video_tracks.size();
        seq.video_tracks.push_back(std::move(extra));
        return place_clip(seq, kind, new_index, std::move(clip), Placement::Overwrite);
    }

    Track* target = seq.track(kind, track_index);
    if (!target || target->locked) return nullptr;

    if (mode == Placement::AppendAtEnd) clip.tl_in = target->end_frame();

    SingleTrackEdit edit(seq, kind, track_index, "place clip");
    clip.tl_out = clip.tl_in + (clip.src_out - clip.src_in);

    if (mode == Placement::Insert) {
        shift_from(target->clips, clip.tl_in, clip.duration());
    } else {
        target->clips = clipped_range(target->clips, clip.tl_in, clip.tl_out);
    }

    clip.id = seq.next_clip_id++;
    target->insert_sorted(std::move(clip));
    return edit.finish();
}

std::unique_ptr<ICommand> place_linked_clip(Sequence& seq, const std::size_t video_track,
                                            const std::size_t audio_track, Clip video, Clip audio,
                                            const Placement mode) {
    Track* vt = seq.track(Track::Kind::Video, video_track);
    Track* at = seq.track(Track::Kind::Audio, audio_track);
    std::size_t vindex = video_track;
    if (mode == Placement::PlaceOnTop) {
        for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
            const Track& t = seq.video_tracks[i];
            bool free = true;
            for (const auto& c : t.clips)
                if (video.tl_in < c.tl_out && c.tl_in < video.tl_out) {
                    free = false;
                    break;
                }
            if (free) { vt = seq.track(Track::Kind::Video, i); vindex = i; break; }
        }
    }
    if (!vt || !at || vt->locked || at->locked) return nullptr;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {Track::Kind::Video, vindex});
    collect_track(involved, seq, {Track::Kind::Audio, audio_track});
    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    if (mode == Placement::AppendAtEnd) {
        video.tl_in = vt->end_frame();
        audio.tl_in = at->end_frame();
    }

    video.id = seq.next_clip_id++;
    audio.id = seq.next_clip_id++;
    video.linked_id = audio.id;
    audio.linked_id = video.id;
    video.tl_out = video.tl_in + (video.src_out - video.src_in);
    audio.tl_out = audio.tl_in + (audio.src_out - audio.src_in);

    if (mode == Placement::Insert) {
        shift_from(vt->clips, video.tl_in, video.duration());
        shift_from(at->clips, audio.tl_in, audio.duration());
    } else {
        vt->clips = clipped_range(vt->clips, video.tl_in, video.tl_out);
        at->clips = clipped_range(at->clips, audio.tl_in, audio.tl_out);
    }

    vt->insert_sorted(std::move(video));
    at->insert_sorted(std::move(audio));

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("place linked clip", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> unlink_clip(Sequence& seq, const Track::Kind kind,
                                      const std::size_t track_index, const ClipId id) {
    Track* t = seq.track(kind, track_index);
    const Clip* c = t ? t->clip_with_id(id) : nullptr;
    if (!c || !c->is_linked()) return nullptr;

    Track* mate_track = nullptr;
    const auto ref = find_clip_ref(seq, c->linked_id, &mate_track);
    if (!ref || !mate_track) return nullptr;
    const ClipId mate_id = c->linked_id;

    std::vector<TrackRef> involved{{kind, track_index}, *ref};
    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    for (auto& cc : t->clips)
        if (cc.id == id) { cc.linked_id = 0; break; }
    for (auto& mc : mate_track->clips)
        if (mc.id == mate_id) { mc.linked_id = 0; break; }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("unlink", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> link_clip(Sequence& seq, const Track::Kind kind,
                                    const std::size_t track_index, const ClipId id) {
    Track* t = seq.track(kind, track_index);
    const Clip* c = t ? t->clip_with_id(id) : nullptr;
    if (!c || c->is_linked()) return nullptr;

    const Track::Kind other = kind == Track::Kind::Video ? Track::Kind::Audio : Track::Kind::Video;

    // Find the unlinked clip of the opposite kind whose time range best overlaps
    // the source clip's range.
    const Clip* mate = nullptr;
    std::size_t mate_track = 0;
    std::size_t best_overlap = 0;
    for (std::size_t i = 0; i < seq.track_count(other); ++i) {
        for (const auto& cc : seq.track(other, i)->clips) {
            if (cc.linked_id != 0) continue;
            const int64_t ov = std::min(c->tl_out, cc.tl_out) - std::max(c->tl_in, cc.tl_in);
            if (ov <= 0) continue;
            if (static_cast<std::size_t>(ov) > best_overlap) {
                best_overlap = static_cast<std::size_t>(ov);
                mate = &cc;
                mate_track = i;
            }
        }
    }
    if (!mate) return nullptr;

    std::vector<TrackRef> involved{{kind, track_index}, {other, mate_track}};
    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    const ClipId mate_id = mate->id;
    for (auto& cc : t->clips)
        if (cc.id == id) { cc.linked_id = mate_id; break; }
    for (auto& mc : seq.track(other, mate_track)->clips)
        if (mc.id == mate_id) { mc.linked_id = id; break; }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("link", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> lift_range(Sequence& seq, const Track::Kind kind,
                                     const std::size_t track_index, const int64_t in,
                                     const int64_t out) {
    Track* target = seq.track(kind, track_index);
    if (!target || target->locked) return nullptr;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {kind, track_index});

    std::vector<ClipId> linked_to_remove;
    for (const auto& c : target->clips)
        if (c.tl_out > in && c.tl_in < out && c.linked_id != 0)
            linked_to_remove.push_back(c.linked_id);
    for (const ClipId lid : linked_to_remove) {
        Track* mt = nullptr;
        if (const auto ref = find_clip_ref(seq, lid, &mt))
            collect_track(involved, seq, *ref);
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    target->clips = clipped_range(target->clips, in, out);

    for (const ClipId lid : linked_to_remove) {
        Track* mt = nullptr;
        const auto ref = find_clip_ref(seq, lid, &mt);
        if (!ref || !mt || mt->locked) continue;
        mt->clips.erase(std::remove_if(mt->clips.begin(), mt->clips.end(),
                                       [lid](const Clip& c) { return c.id == lid; }),
                        mt->clips.end());
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("lift", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> ripple_delete_range(Sequence& seq, const Track::Kind kind,
                                              const std::size_t track_index, const int64_t in,
                                              const int64_t out) {
    Track* target = seq.track(kind, track_index);
    if (!target || target->locked) return nullptr;
    SingleTrackEdit edit(seq, kind, track_index, "ripple delete");
    const int64_t gap = out - in;
    target->clips = clipped_range(target->clips, in, out);
    shift_from(target->clips, in, -gap);
    return edit.finish();
}

std::unique_ptr<ICommand> blade_at(Sequence& seq, const Track::Kind kind,
                                   const std::size_t track_index, const int64_t pos) {
    Track* target = seq.track(kind, track_index);
    const Clip* hit = target ? target->clip_at(pos) : nullptr;
    if (!target || target->locked || !hit) return nullptr;
    if (pos <= hit->tl_in || pos >= hit->tl_out) return nullptr;

    SingleTrackEdit edit(seq, kind, track_index, "blade");

    Clip right = *hit;
    right.tl_in = pos;
    right.src_in = hit->src_in + (pos - hit->tl_in);
    right.id = seq.next_clip_id++;

    for (auto& c : target->clips)
        if (c.id == hit->id) {
            c.tl_out = pos;
            c.src_out = c.src_in + c.duration();
            break;
        }
    target->insert_sorted(std::move(right));
    return edit.finish();
}

std::unique_ptr<ICommand> blade_linked_at(Sequence& seq, const Track::Kind kind,
                                          const std::size_t track_index, const int64_t pos) {
    Track* target = seq.track(kind, track_index);
    const Clip* hit = target ? target->clip_at(pos) : nullptr;
    if (!target || target->locked || !hit) return nullptr;
    if (pos <= hit->tl_in || pos >= hit->tl_out) return nullptr;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {kind, track_index});

    Track* mate_track = nullptr;
    const Clip* mate = nullptr;
    if (hit->is_linked()) {
        if (const auto ref = find_clip_ref(seq, hit->linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate = mate_track->clip_with_id(hit->linked_id);
        }
    }

    CANVAS_LOG("blade: kind=%d track=%zu pos=%lld clip=%lld [%lld,%lld) linked=%lld",
           (int)kind, track_index, (long long)pos, (long long)hit->id,
           (long long)hit->tl_in, (long long)hit->tl_out, (long long)hit->linked_id);
    if (mate) {
        CANVAS_LOG("blade:   linked mate on kind=A clip=%lld [%lld,%lld); re-pair right halves -> %lld<->%lld",
               (long long)mate->id, (long long)mate->tl_in, (long long)mate->tl_out,
               (long long)hit->linked_id, (long long)mate->linked_id);
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    // Cut the primary clip.
    Clip right = *hit;
    right.tl_in = pos;
    right.src_in = hit->src_in + (pos - hit->tl_in);
    right.id = seq.next_clip_id++;

    // Cut the linked mate at the same position, if present and unlocked.
    std::optional<Clip> mright;
    if (mate_track && mate && !mate_track->locked && pos > mate->tl_in && pos < mate->tl_out) {
        mright = *mate;
        mright->tl_in = pos;
        mright->src_in = mate->src_in + (pos - mate->tl_in);
        mright->id = seq.next_clip_id++;
    }

    // Shorten the left halves in place. Their links (left video <-> left audio)
    // are unchanged and remain correct.
    for (auto& c : target->clips)
        if (c.id == hit->id) {
            c.tl_out = pos;
            c.src_out = c.src_in + c.duration();
            break;
        }
    if (mright) {
        for (auto& c : mate_track->clips)
            if (c.id == mate->id) {
                c.tl_out = pos;
                c.src_out = c.src_in + c.duration();
                break;
            }
        // Re-pair the two right halves so they link to each other rather than
        // both pointing back at the left halves.
        right.linked_id = mright->id;
        mright->linked_id = right.id;
        mate_track->insert_sorted(std::move(*mright));
    }

    target->insert_sorted(std::move(right));

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("blade linked", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> lift_clip(Sequence& seq, const Track::Kind kind,
                                    const std::size_t track_index, const ClipId id) {
    Track* target = seq.track(kind, track_index);
    const Clip* c = target ? target->clip_with_id(id) : nullptr;
    if (!c || target->locked) return nullptr;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {kind, track_index});
    if (c->is_linked()) {
        Track* mt = nullptr;
        if (const auto ref = find_clip_ref(seq, c->linked_id, &mt)) {
            collect_track(involved, seq, *ref);
            if (mt && mt->locked) return nullptr;
        }
    }

    CANVAS_LOG("lift: kind=%d track=%zu id=%lld tl=[%lld,%lld) linked=%lld",
           (int)kind, track_index, (long long)id, (long long)c->tl_in,
           (long long)c->tl_out, (long long)c->linked_id);

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    // Capture everything we need from `c` *before* erasing from `target->clips`:
    // erase() may free/reallocate the vector, leaving `c` dangling. Reading
    // `c->linked_id` after the erase is use-after-free and can remove a
    // *different* clip than the intended mate (e.g. the clip to the right of a
    // split pair).
    const int64_t in = c->tl_in, out = c->tl_out;
    const bool linked = c->is_linked();
    const ClipId lid = c->linked_id;
    target->clips.erase(
        std::remove_if(target->clips.begin(), target->clips.end(),
                       [id](const Clip& x) { return x.id == id; }),
        target->clips.end());
    if (linked) {
        Track* mt = nullptr;
        if (find_clip_ref(seq, lid, &mt)) {
            mt->clips.erase(std::remove_if(mt->clips.begin(), mt->clips.end(),
                                           [lid](const Clip& x) { return x.id == lid; }),
                            mt->clips.end());
        }
        (void)in; (void)out;
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    log_deleted_clips(before, after);
    CANVAS_LOG("lift: done id=%lld -> created command", (long long)id);
    return std::make_unique<EditCommand>("lift", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> ripple_delete_clip(Sequence& seq, const Track::Kind kind,
                                             const std::size_t track_index, const ClipId id) {
    Track* target = seq.track(kind, track_index);
    const Clip* c = target ? target->clip_with_id(id) : nullptr;
    if (!c || target->locked) return nullptr;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {kind, track_index});

    Track* mate_track = nullptr;
    const Clip* mate = nullptr;
    if (c->is_linked()) {
        if (const auto ref = find_clip_ref(seq, c->linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate = mate_track->clip_with_id(c->linked_id);
        }
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    const int64_t in = c->tl_in, out = c->tl_out;
    const int64_t gap = out - in;

    CANVAS_LOG("ripple: kind=%d track=%zu id=%lld tl=[%lld,%lld) gap=%lld linked=%lld mate=%s",
           (int)kind, track_index, (long long)id, (long long)in, (long long)out,
           (long long)gap, (long long)c->linked_id, mate ? "yes" : "no");

    target->clips.erase(
        std::remove_if(target->clips.begin(), target->clips.end(),
                       [id](const Clip& x) { return x.id == id; }),
        target->clips.end());
    shift_from(target->clips, in, -gap);

    if (mate_track && mate) {
        mate_track->clips.erase(
            std::remove_if(mate_track->clips.begin(), mate_track->clips.end(),
                           [&](const Clip& x) { return x.id == mate->id; }),
            mate_track->clips.end());
        // When the mate lives on a *different* track (typical video<->audio
        // link) the primary track was already rippled above; ripple the mate's
        // track by the same gap so the pair stays in sync. A same-track mate
        // shares the already-rippled track, so it is not shifted again.
        if (mate_track != target) shift_from(mate_track->clips, in, -gap);
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    log_deleted_clips(before, after);
    CANVAS_LOG("ripple: done id=%lld gap=%lld mate_rippled=%s",
           (long long)id, (long long)gap, (mate_track && mate_track != target) ? "yes" : "no");
    return std::make_unique<EditCommand>("ripple delete", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> move_clip(Sequence& seq, const Track::Kind src_kind,
                                    const std::size_t src_track, const ClipId id,
                                    const Track::Kind dst_kind, const std::size_t dst_track,
                                    const int64_t new_tl_in) {
    Track* src = seq.track(src_kind, src_track);
    Track* dst = seq.track(dst_kind, dst_track);
    const Clip* moving = src && dst ? src->clip_with_id(id) : nullptr;
    if (!moving || src->locked || dst->locked) return nullptr;

    const bool same = src_kind == dst_kind && src_track == dst_track;
    const Clip copy = *moving;
    const int64_t dur = copy.duration();
    const int64_t delta = new_tl_in - copy.tl_in;

    std::vector<TrackRef> involved;
    collect_track(involved, seq, {src_kind, src_track});
    if (!same) collect_track(involved, seq, {dst_kind, dst_track});

    Track* mate_track = nullptr;
    const Clip* mate = nullptr;
    if (copy.linked_id != 0) {
        if (const auto ref = find_clip_ref(seq, copy.linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate = mate_track->clip_with_id(copy.linked_id);
        }
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    src->clips.erase(
        std::remove_if(src->clips.begin(), src->clips.end(), [id](const Clip& c) { return c.id == id; }),
        src->clips.end());

    Clip placed = copy;
    placed.tl_in = new_tl_in;
    placed.tl_out = new_tl_in + dur;
    dst->clips = clipped_range(dst->clips, placed.tl_in, placed.tl_out);
    dst->insert_sorted(std::move(placed));

    if (mate_track && mate) {
        const int64_t mate_new = std::max<int64_t>(0, mate->tl_in + delta);
        Clip mm = *mate;
        const int64_t mdur = mm.duration();
        mate_track->clips.erase(
            std::remove_if(mate_track->clips.begin(), mate_track->clips.end(),
                           [&](const Clip& c) { return c.id == mate->id; }),
            mate_track->clips.end());
        mm.tl_in = mate_new;
        mm.tl_out = mate_new + mdur;
        mate_track->clips = clipped_range(mate_track->clips, mm.tl_in, mm.tl_out);
        mate_track->insert_sorted(std::move(mm));
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>("move", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> create_top_track_move(Sequence& seq, const ClipId id,
                                                const int64_t new_tl_in) {
    Track* src = nullptr;
    Track::Kind src_kind = Track::Kind::Video;
    std::size_t src_index = 0;
    const Clip* primary = nullptr;
    for (int ki = 0; ki < 2 && !primary; ++ki) {
        const Track::Kind k = ki == 0 ? Track::Kind::Video : Track::Kind::Audio;
        for (std::size_t i = 0; i < seq.track_count(k); ++i) {
            Track* t = seq.track(k, i);
            if (t->clip_with_id(id)) {
                primary = t->clip_with_id(id);
                src = t;
                src_kind = k;
                src_index = i;
                break;
            }
        }
    }
    if (!primary || !src || src->locked || seq.video_tracks.empty() || seq.audio_tracks.empty())
        return nullptr;

    std::optional<TrackRef> mate_ref;
    const ClipId mate_id = primary->linked_id;
    if (mate_id != 0) mate_ref = find_clip_ref(seq, mate_id, nullptr);

    const Clip copy = *primary;
    const int64_t prim_old_tl_in = copy.tl_in;
    const int64_t dur = copy.duration();

    // Insert brand-new topmost channels; the moved clips land on an empty lane,
    // so no destination clipping is ever needed.
    Track new_video;
    new_video.kind = Track::Kind::Video;
    new_video.name = "V" + std::to_string(seq.track_count(Track::Kind::Video) + 1);
    Track new_audio;
    new_audio.kind = Track::Kind::Audio;
    new_audio.name = "A" + std::to_string(seq.track_count(Track::Kind::Audio) + 1);
    seq.video_tracks.insert(seq.video_tracks.begin(), std::move(new_video));
    seq.audio_tracks.insert(seq.audio_tracks.begin(), std::move(new_audio));

    std::vector<TrackRef> refs;
    for (int ki = 0; ki < 2; ++ki) {
        const Track::Kind k = ki == 0 ? Track::Kind::Video : Track::Kind::Audio;
        for (std::size_t i = 0; i < seq.track_count(k); ++i) refs.push_back({k, i});
    }
    std::vector<TrackSnapshot> before = take_snapshots(seq, refs);

    // The insertion reallocated the track vectors; re-resolve the live tracks
    // (each original lane shifted down by one) before editing them.
    Track* live_src = seq.track(src_kind, src_index + 1);
    Track* live_mate = mate_ref ? seq.track(mate_ref->kind, mate_ref->index + 1) : nullptr;
    if (!live_src) return nullptr;

    // Move the primary onto the new video lane at new_tl_in.
    live_src->clips.erase(
        std::remove_if(live_src->clips.begin(), live_src->clips.end(),
                       [id](const Clip& c) { return c.id == id; }),
        live_src->clips.end());
    Clip moved = copy;
    moved.tl_in = new_tl_in;
    moved.tl_out = new_tl_in + dur;
    seq.track(Track::Kind::Video, 0)->insert_sorted(std::move(moved));

    // Move the linked mate to the new audio lane, preserving A/V sync.
    if (live_mate) {
        const Clip* live_mate_clip = live_mate->clip_with_id(mate_id);
        if (live_mate_clip) {
            const int64_t mate_new = new_tl_in + (live_mate_clip->tl_in - prim_old_tl_in);
            live_mate->clips.erase(
                std::remove_if(live_mate->clips.begin(), live_mate->clips.end(),
                               [&](const Clip& c) { return c.id == live_mate_clip->id; }),
                live_mate->clips.end());
            Clip mm = *live_mate_clip;
            mm.tl_in = mate_new;
            mm.tl_out = mate_new + live_mate_clip->duration();
            seq.track(Track::Kind::Audio, 0)->insert_sorted(std::move(mm));
        }
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, refs);
    return std::make_unique<EditCommand>("auto-track", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> set_clip_enabled(Sequence& seq, const Track::Kind kind,
                                           const std::size_t track_index, const ClipId id,
                                           const bool enabled) {
    Track* t = seq.track(kind, track_index);
    const Clip* c = t ? t->clip_with_id(id) : nullptr;
    if (!c) return nullptr;

    std::vector<TrackRef> involved{{kind, track_index}};

    Track* mate_track = nullptr;
    ClipId mate_id = 0;
    if (c->linked_id != 0) {
        if (const auto ref = find_clip_ref(seq, c->linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate_id = c->linked_id;
        }
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    for (auto& cc : t->clips)
        if (cc.id == id) { cc.enabled = enabled; break; }
    if (mate_track && mate_id != 0) {
        for (auto& mc : mate_track->clips)
            if (mc.id == mate_id) { mc.enabled = enabled; break; }
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    return std::make_unique<EditCommand>(
        enabled ? "enable clip" : "disable clip", std::move(before), std::move(after));
}

std::unique_ptr<ICommand> set_clip_transition(Sequence& seq, const Track::Kind kind,
                                              const std::size_t track_index, const ClipId id,
                                              const TransitionType type, const int64_t duration) {
    return set_clip_transition_edge(seq, kind, track_index, id, TransitionEdge::Out, type, duration);
}

std::unique_ptr<ICommand> set_clip_transition_in(Sequence& seq, const Track::Kind kind,
                                                 const std::size_t track_index, const ClipId id,
                                                 const TransitionType type, const int64_t duration) {
    return set_clip_transition_edge(seq, kind, track_index, id, TransitionEdge::In, type, duration);
}

std::unique_ptr<ICommand> clear_clip_transition(Sequence& seq, const Track::Kind kind,
                                                const std::size_t track_index, const ClipId id) {
    return set_clip_transition(seq, kind, track_index, id, TransitionType::None, 0);
}

std::unique_ptr<ICommand> clear_clip_transition_in(Sequence& seq, const Track::Kind kind,
                                                   const std::size_t track_index, const ClipId id) {
    return set_clip_transition_in(seq, kind, track_index, id, TransitionType::None, 0);
}

std::unique_ptr<ICommand> delete_through_edit(Sequence& seq, const Track::Kind kind,
                                              const std::size_t track_index, const ClipId out_id) {
    Track* t = seq.track(kind, track_index);
    const Clip* a = t ? t->clip_with_id(out_id) : nullptr;
    if (!a) {
        CANVAS_LOG("delete_through_edit: kind=%d track=%zu out_id=%lld NOT FOUND",
               static_cast<int>(kind), track_index, (long long)out_id);
        return nullptr;
    }

    // The incoming clip B starts exactly where A ends.
    const Clip* b = nullptr;
    for (const auto& c : t->clips) {
        if (&c != a && c.tl_in == a->tl_out) { b = &c; break; }
    }
    if (!b) {
        CANVAS_LOG("delete_through_edit: kind=%d track=%zu out_id=%lld tl_out=%lld has NO adjacent next clip",
               static_cast<int>(kind), track_index, (long long)out_id, (long long)a->tl_out);
        return nullptr;
    }
    // A genuine "through" edit joins continuous source from the same media.
    if (a->media != b->media || b->src_in != a->src_out || b->tl_out <= a->tl_out) {
        CANVAS_LOG("delete_through_edit: kind=%d track=%zu OUT=%lld media=%d [%lld,%lld) -> IN=%lld media=%d "
               "src_in=%lld src_out=%lld REJECTED (not a continuous through edit)",
               static_cast<int>(kind), track_index, (long long)out_id, (int)a->media,
               (long long)a->tl_out, (long long)a->tl_out, (long long)b->id, (int)b->media,
               (long long)b->src_in, (long long)b->src_out);
        return nullptr;
    }

    std::vector<TrackRef> involved{{kind, track_index}};

    // If A is linked, we must also break the mate link so the merged clip does
    // not dangle an A/V pairing that no longer matches its span.
    Track* mate_track = nullptr;
    ClipId mate_id = 0;
    if (a->linked_id != 0) {
        if (const auto ref = find_clip_ref(seq, a->linked_id, &mate_track)) {
            collect_track(involved, seq, *ref);
            mate_id = a->linked_id;
        }
    }

    std::vector<TrackSnapshot> before = take_snapshots(seq, involved);

    // Build the merged clip: A's identity/metadata extended over B's span. The
    // outgoing transition that lived on the removed cut is dropped.
    Clip merged = *a;
    merged.tl_out = b->tl_out;
    merged.src_out = b->src_out;
    merged.transition_out = TransitionType::None;
    merged.transition_out_duration = 0;
    merged.linked_id = 0;

    t->clips.erase(std::remove_if(t->clips.begin(), t->clips.end(),
                                  [&](const Clip& c) { return (&c == a) || (&c == b); }),
                   t->clips.end());
    t->insert_sorted(std::move(merged));

    if (mate_track && mate_id != 0) {
        for (auto& mc : mate_track->clips)
            if (mc.id == mate_id) { mc.linked_id = 0; break; }
    }

    std::vector<TrackSnapshot> after = take_snapshots(seq, involved);
    CANVAS_LOG("delete_through_edit: MERGED out=%lld + in=%lld on kind=%d track=%zu -> id=%lld tl=[%lld,%lld) "
           "src=[%lld,%lld) dropped_transition=%s unlinked_mate=%lld",
           (long long)out_id, (long long)b->id, static_cast<int>(kind), track_index,
           (long long)merged.id, (long long)merged.tl_in, (long long)merged.tl_out,
           (long long)merged.src_in, (long long)merged.src_out,
           (a->has_transition() ? "yes" : "no"), (long long)mate_id);
    return std::make_unique<EditCommand>("delete through edit", std::move(before), std::move(after));
}

}
