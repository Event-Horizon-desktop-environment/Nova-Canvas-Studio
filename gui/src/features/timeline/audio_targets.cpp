#include "features/timeline/audio_targets.hpp"

namespace canvas::gui {

std::vector<AudioTarget> resolve_audio_targets(
    const canvas::core::Sequence& seq,
    const std::vector<canvas::core::ClipId>& ids) {
    std::vector<AudioTarget> out;
    for (const canvas::core::ClipId id : ids) {
        const canvas::core::Clip* audio_clip = nullptr;
        std::size_t audio_track = 0;

        // Audio clip in the selection: target it directly.
        for (std::size_t t = 0; t < seq.audio_tracks.size(); ++t) {
            if (const auto* c = seq.audio_tracks[t].clip_with_id(id)) {
                audio_clip = c;
                audio_track = t;
                break;
            }
        }

        // Video clip: drive its linked audio mate (if any).
        if (!audio_clip) {
            for (std::size_t v = 0; v < seq.video_tracks.size() && !audio_clip; ++v) {
                const auto* vc = seq.video_tracks[v].clip_with_id(id);
                if (!vc || vc->linked_id == 0) continue;
                for (std::size_t t = 0; t < seq.audio_tracks.size(); ++t) {
                    if (const auto* c = seq.audio_tracks[t].clip_with_id(vc->linked_id)) {
                        audio_clip = c;
                        audio_track = t;
                        break;
                    }
                }
            }
        }
        if (!audio_clip) continue;

        // Linked pairs were already expanded by the caller (or reach each other
        // through linked_id from either half) — emit each audio clip once.
        bool dup = false;
        for (const auto& e : out) {
            if (e.id == audio_clip->id) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        out.push_back(AudioTarget{canvas::core::Track::Kind::Audio, audio_track,
                                  audio_clip->id, *audio_clip});
    }
    return out;
}

}  // namespace canvas::gui