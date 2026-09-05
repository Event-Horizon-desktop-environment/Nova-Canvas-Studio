#include "audio_pipeline.hpp"

#include "audio_sink.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace canvas::gui {

bool AudioPipeline::is_open() const { return sink_.is_open(); }

using Clock = std::chrono::steady_clock;

namespace {
// Mirrors canvas::gui::playback_debug() (CANVAS_PLAYBACK_DEBUG) without Qt.
bool playback_dbg() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_PLAYBACK_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

double media_fps_of(const canvas::core::Project& project, const canvas::core::Clip& clip) {
    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                 [&](const canvas::core::MediaEntry& m) { return m.id == clip.media; });
    return (it != project.media.end() && it->fps > 0.0) ? it->fps : 0.0;
}

// Applies a clip's audio IN/OUT transition envelopes to decoded PCM before it
// reaches the output device, so linked-pair transitions (video type translated
// to an AudioFade* on the audio mate) and manually-added audio fades are
// actually AUDIBLE in playback — matching what the exporter's audio mix does.
// Returns the chunk's own samples when no audio fade touches the written
// tl-frame range (the common case: zero copies); otherwise a per-frame gain-
// scaled copy in `scratch`.
const std::vector<float>* apply_audio_fades(const canvas::core::Clip& clip,
                                            const canvas::core::AudioChunkPtr& c,
                                            const int64_t first_media_sample, const int out_rate,
                                            const double fps_v, std::vector<float>* scratch) {
    scratch->clear();
    const int ch = c->channels > 0 ? c->channels : 1;
    const int frames = static_cast<int>(c->samples.size() / static_cast<std::size_t>(ch));
    if (frames <= 0) return &c->samples;
    const bool has_fade =
        (clip.transition_in_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_in)) ||
        (clip.transition_out_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_out));
    if (!has_fade) return &c->samples;

    std::vector<float> gains(static_cast<std::size_t>(frames));
    bool touched = false;
    for (int k = 0; k < frames; ++k) {
        // Frame-granular tl mapping (floor, NOT llround): a sample's timeline
        // frame is floor(sample / out_rate * fps), so the gains land on the same
        // whole frames the renderer's audio_chunk uses (llround would shift the
        // second half of every frame onto the NEXT tl frame).
        const int64_t tl = clip.tl_in + (static_cast<int64_t>(std::floor(
            static_cast<double>(first_media_sample + k) / out_rate * fps_v)) -
                                         clip.src_in);
        const float g = canvas::core::audio_fade_gain(clip, tl);
        gains[static_cast<std::size_t>(k)] = g;
        if (g < 0.999999f) touched = true;
    }
    if (!touched) return &c->samples;
    scratch->resize(c->samples.size());
    const std::size_t ch_u = static_cast<std::size_t>(ch);
    for (std::size_t s = 0; s < c->samples.size(); ++s)
        (*scratch)[s] = c->samples[s] * gains[s / ch_u];
    return scratch;
}
}  // namespace

AudioPipeline::~AudioPipeline() { reset(); }

void AudioPipeline::set_project(const canvas::core::Project* project) {
    std::lock_guard lock(mutex_);
    project_ = project;
}

void AudioPipeline::add_media(const canvas::core::MediaEntry& entry) {
    auto adec = std::make_unique<canvas::core::AudioDecoder>();
    if (adec->open(entry.path) && adec->has_audio()) {
        if (::canvas::core::log::enabled())
            ::canvas::core::log::log_warning("audio: opened decoder for media %d rate=%d ch=%d",
                                         entry.id, adec->source_sample_rate(),
                                         adec->source_channels());
        std::lock_guard lock(mutex_);
        decoders_[entry.id] = std::move(adec);
    } else {
        if (adec->has_audio())
            ::canvas::core::log::log_warning("audio: FAILED to open audio decoder for media %d path=%s",
                                         entry.id, entry.path.c_str());
        else
            ::canvas::core::log::log_warning("audio: media %d has no audio stream (or open failed) path=%s",
                                         entry.id, entry.path.c_str());
    }
}

void AudioPipeline::reset() {
    std::lock_guard lock(mutex_);
    if (active_) {
        sink_.flush();
        active_ = false;
    }
    decoders_.clear();
    project_ = nullptr;
    anchor_media_sample_ = 0;
    written_at_anchor_ = 0;
    run_id_ = 0;
    speed_run_ = 0;
    speed_video_ms_ = 0.0;
    speed_audible_ms_ = 0.0;
    feed_sample_ = -1;
    feed_media_ = -1;
    last_scrub_audio_target_ = -1;
    last_scrub_audio_at_ = {};
    scrub_repositions_ = 0;
}

void AudioPipeline::open_output() {
    std::lock_guard lock(mutex_);
    if (active_) return;
    active_ = sink_.open(rate_, channels_);
    if (!active_)
        ::canvas::core::log::log_warning("audio: FAILED to open output device rate=%d channels=%d",
                                     rate_, channels_);
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: pipeline open output active=%d rate=%d channels=%d",
                                     (int)active_, rate_, channels_);
}

void AudioPipeline::close_output() {
    std::lock_guard lock(mutex_);
    if (!active_) return;
    sink_.flush();
    sink_.close();
    active_ = false;
}

void AudioPipeline::rewind(int64_t seq_frame, bool playing) {
    std::lock_guard lock(mutex_);
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: pipeline rewind active=%d playing=%d", (int)active_,
                                     (int)playing);
    // Mark a new audio run so the sync log can compare A/V offset before vs
    // after the re-anchor (this is the boundary the user wants to see).
    ++run_id_;
    ::canvas::core::log::log_warning("[avsync] RE-ANCHOR run=%llu at_seq_frame=%lld device_written=%llu",
                                 static_cast<unsigned long long>(run_id_),
                                 static_cast<long long>(seq_frame),
                                 static_cast<unsigned long long>(sink_.stat_written_frames()));
    // Remember the media sample the next audio run starts writing from, and the
    // device's written-frame counter at this instant, so we can later derive the
    // audible position within this run for A/V sync diagnostics.
    written_at_anchor_ = sink_.stat_written_frames();
    anchor_media_sample_ = playhead_to_audio_sample(seq_frame);
    // A fresh play run: nothing has been fed to the device yet, so the feed
    // watermark starts empty and the next (pre)roll/(play)step starts at the
    // anchor. Prevents pre-rolled audio from being handed to the driver twice.
    feed_sample_ = -1;
    feed_media_ = -1;
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: rewind reset feed, anchor_media_sample=%lld rate=%d",
                                     static_cast<long long>(anchor_media_sample_), rate_);
    for (auto& [id, adec] : decoders_) adec->reset();
    // Re-arm the output device on every seek so playback restarts from the new
    // playhead position. If we leave the device holding samples decoded at the
    // old position, a subsequent Play resumes stale/misaligned audio instead of
    // the fresh position. flush() drains, prepares and restarts the writer.
    if (active_) {
        sink_.log_pipeline_stats("rewind-pre");
        sink_.flush();
        sink_.log_pipeline_stats("rewind-post");
    }
}

// Write `lead_ms` of audio for the current playhead into the output before any
// video frame is presented. The ALSA device holds a fixed buffer latency (here
// ~50ms buffer plus writer headroom, observed ~70ms), so audio written at the
// same moment a frame is shown is HEARD ~70ms later — audio lags video by that
// constant. Pre-filling the device with leading audio puts the AUDIBLE cursor
// (written minus latency) on the picture, so sync lands at ~0 instead of ~70ms.
void AudioPipeline::preroll(int64_t seq_frame, int lead_ms, bool playing) {
    std::lock_guard lock(mutex_);
    if (!active_ || !sink_.is_open() || lead_ms <= 0) return;
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) return;
    auto ait = decoders_.find(clip->media);
    if (ait == decoders_.end() || !ait->second->has_audio()) return;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return;
    const double step_s = 1.0 / fps_v;
    const int64_t step_frames =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(step_s * rate_)));
    const int64_t base_sample = playhead_to_audio_sample(seq_frame);
    const int64_t total = static_cast<int64_t>(static_cast<double>(lead_ms) / 1000.0 * rate_);
    // The pre-roll writes the first `total` media samples at the anchor. Advance
    // the feed watermark so play_step() starts AFTER this leading audio instead
    // of re-writing it (which pulled the audible cursor ahead of video).
    feed_media_ = clip->media;
    feed_sample_ = std::max(feed_sample_, base_sample);
    int64_t written = 0;
    std::vector<float> fade_scratch;
    while (written < total) {
        const int64_t chunk = std::min(step_frames, total - written);
        auto s = ait->second->decode(base_sample + written, static_cast<int>(chunk), rate_);
        if (!s || s->samples.empty()) break;
        const int f = static_cast<int>(s->samples.size() / s->channels);
        if (f <= 0) break;
        const std::vector<float>* data =
            apply_audio_fades(*clip, s, base_sample + written, rate_, fps_v, &fade_scratch);
        sink_.write_float(data->data(), f);
        written += f;
    }
    feed_sample_ = std::max(feed_sample_, base_sample + written);
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: preroll lead_ms=%d written_frames=%lld media_sample=%lld "
                                     "feed_sample=%lld clip_media=%d",
                                     lead_ms, static_cast<long long>(written),
                                     static_cast<long long>(base_sample),
                                     static_cast<long long>(feed_sample_), clip->media);
    ::canvas::core::log::log_warning(
        "[audio:preroll] cur=%lld base_sample=%lld written_frames=%lld audible_before=%llu playing=%d",
        static_cast<long long>(seq_frame), static_cast<long long>(base_sample),
        static_cast<long long>(written),
        static_cast<unsigned long long>(sink_.audible_position_frames()), (int)playing);
}

// Audible scrub: decode a short PCM grain from the media at `seq_frame` and write
// it to the already-open output device. A short (~40ms) slice keeps each grain
// distinct and cheap; best-effort (returns on any missing component). Mirrors
// preroll's decode+write pattern but for a discrete scrub "blip" rather than a
// continuous run. The device stays open across the whole drag so we never pay a
// per-move open/flush (the reported stutter source).
void AudioPipeline::play_scrub_grain(int64_t seq_frame) {
    std::lock_guard lock(mutex_);
    if (!project_ || !active_ || !sink_.is_open()) return;
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) {
        if (project_) {
            if (const auto* present = clip_at_any_track(seq_frame))
                ::canvas::core::log::log_warning(
                    "[scrub] GHOST-GUARD frame=%lld clip covers frame enabled=%d media=%d tl=%lld->%lld",
                    static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                    static_cast<long long>(present->tl_in),
                    static_cast<long long>(present->tl_out));
        }
        return;
    }
    auto ait = decoders_.find(clip->media);
    if (ait == decoders_.end() || !ait->second->has_audio()) return;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return;
    const int64_t base_sample = playhead_to_audio_sample(seq_frame);
    const int64_t grain_frames =
        static_cast<int64_t>(static_cast<double>(40) / 1000.0 * rate_);  // ~40ms
    const auto g0 = Clock::now();
    auto s = ait->second->decode(base_sample, static_cast<int>(grain_frames), rate_);
    const double dec_ms = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
    if (!s || s->samples.empty()) return;
    const int f = static_cast<int>(s->samples.size() / s->channels);
    if (f <= 0) return;
    std::vector<float> fade_scratch;
    const std::vector<float>* data =
        apply_audio_fades(*clip, s, base_sample, rate_, fps_v, &fade_scratch);
    if (!sink_.write_float(data->data(), f))
        ::canvas::core::log::log_warning("[scrub] GRAIN dropped (overflow) at frame=%lld frames=%d",
                                     static_cast<long long>(seq_frame), f);
    if (dec_ms > 1.0)
        ::canvas::core::log::log_warning("[scrub] GRAIN frame=%lld decode_ms=%.2f samples=%zu",
                                     static_cast<long long>(seq_frame), dec_ms,
                                     s->samples.size());
}

const canvas::core::Clip* AudioPipeline::audio_clip_at(int64_t seq_frame) const {
    if (!project_ || seq_frame < 0 || seq_frame >= project_->sequence.duration_frames())
        return nullptr;
    const canvas::core::Sequence& seq = project_->sequence;
    // An audio-track clip that covers the playhead decides the output for this
    // frame: if enabled it plays, if disabled it is an intentional mute. In
    // either case we must NOT fall through to the video track — otherwise a
    // disabled (e.g. unlinked-then-muted) audio clip would be overridden by the
    // movie's embedded audio on the video track and Ctrl+D would appear broken.
    for (std::size_t i = seq.audio_tracks.size(); i-- > 0;) {
        const auto& track = seq.audio_tracks[i];
        if (track.locked) continue;
        const canvas::core::Clip* clip = track.clip_at(seq_frame);
        if (clip) return clip->enabled ? clip : nullptr;
    }
    for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
        const auto& track = seq.video_tracks[i];
        if (track.locked) continue;
        const canvas::core::Clip* clip = track.clip_at(seq_frame);
        if (clip && clip->enabled) return clip;
    }
    return nullptr;
}

const canvas::core::Clip* AudioPipeline::clip_at_any_track(int64_t seq_frame) const {
    if (!project_) return nullptr;
    for (const auto& t : project_->sequence.audio_tracks)
        if (const canvas::core::Clip* c = t.clip_at(seq_frame)) return c;
    for (const auto& t : project_->sequence.video_tracks)
        if (const canvas::core::Clip* c = t.clip_at(seq_frame)) return c;
    return nullptr;
}

// Media-time audio sample index for a timeline frame. Matches the computation in
// play_step() so diagnostics and playback agree.
int64_t AudioPipeline::playhead_to_audio_sample(int64_t seq_frame) const {
    if (!project_ || seq_frame < 0) return 0;
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) return 0;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return 0;
    const int64_t src_frame = clip->src_in + (seq_frame - clip->tl_in);
    return static_cast<int64_t>(std::llround(src_frame / fps_v * rate_));
}

int64_t AudioPipeline::audio_sample_to_seq_frame(int64_t media_sample) const {
    if (!project_ || media_sample < 0) return -1;
    // Find an audio clip whose [src_in, src_out) media-frame range covers
    // media_sample and map back to its timeline placement. Audio tracks are few;
    // a linear scan over audio tracks/clips only happens on drop-ahead (once per
    // slow present).
    //
    // Inverts playhead_to_audio_sample(): that computes
    //   src_frame = c.src_in + (seq_frame - c.tl_in)
    //   media_sample = round(src_frame / fps_v * audio_rate_)
    // so here we must first convert media_sample back to src_frame (an ABSOLUTE
    // media-frame position that already includes c.src_in), then subtract
    // c.src_in before adding c.tl_in. The previous version compared media_sample
    // (sample-rate domain) directly against c.src_in (frame-rate domain, a unit
    // mismatch) and then added c.src_in a second time when forming
    // src_frame/seq_frame, so for any trimmed clip (c.src_in > 0 — the normal
    // case) the returned seq_frame was too high by c.src_in frames. That inflated
    // the "audible" frame fed into SonicSync::reconcile's master-clock cap in the
    // drop-to-realtime catch-up — the observed forward-scrub A/V desync.
    const canvas::core::Sequence& seq = project_->sequence;
    for (const canvas::core::Track& t : seq.audio_tracks) {
        for (const canvas::core::Clip& c : t.clips) {
            if (c.media < 0 || !c.enabled) continue;
            const double fps_v = media_fps_of(*project_, c);
            if (fps_v <= 0.0) continue;
            const int64_t src_frame = static_cast<int64_t>(
                std::llround(static_cast<double>(media_sample) / rate_ * fps_v));
            if (src_frame < c.src_in || src_frame >= c.src_out) continue;  // outside this clip's trim
            const int64_t seq_frame = c.tl_in + (src_frame - c.src_in);
            return seq_frame;
        }
    }
    return -1;
}

// Debug output for A/V sync: for the frame being presented NOW, report the
// video's media position (frame/fps) versus the audio position that is CURRENTLY
// AUDIBLE at the speaker (anchored to the last rewind so seeks/scrubs do not
// corrupt the comparison). Logs once per second.
//
//   video_ms     = media time of the picture on screen
//   audible_ms   = media time of the sound coming out of the speaker
//   av_offset_ms = video_ms - audible_ms  (>0: audio heard is BEHIND the picture)
//
// Steady play: a small, roughly constant positive offset (~device buffer latency)
// is expected. A value that GROWS over a segment means real drift. A jump (e.g.
// offset swings massively negative) right after a scrub means the audio wasn't
// re-anchored to the new playhead.
//
// NOTE: does NOT lock mutex_ — the caller (play_step) already holds it, and
// taking it here deadlocks the worker thread on the very first frame.
void AudioPipeline::log_av_sync(int64_t seq_frame, double video_fps, double step_seconds) {
    static auto last_av = Clock::now();
    const auto now = Clock::now();
    if (now - last_av < std::chrono::seconds(1)) return;
    last_av = now;

    const double video_ms = seq_frame / video_fps * 1000.0;
    // Audible position within the current audio run.
    const uint64_t audible_frames = sink_.audible_position_frames();
    const int64_t run_audible =
        static_cast<int64_t>(audible_frames) - static_cast<int64_t>(written_at_anchor_);
    const double audible_ms =
        static_cast<double>(anchor_media_sample_ + run_audible) / static_cast<double>(rate_) * 1000.0;

    // Also report where audio is being WRITTEN (media time of samples being fed
    // to the queue right now), so we can see the write-side vs play-side gap.
    const double written_ms =
        static_cast<double>(anchor_media_sample_ +
                            static_cast<int64_t>(sink_.stat_written_frames()) -
                            static_cast<int64_t>(written_at_anchor_)) /
        static_cast<double>(rate_) * 1000.0;

    // AUDIO SPEED within this run: ratio of audible-media-time advance to
    // video-media-time advance over the last ~1s. 1.00 = correct pace; >1.00 =
    // audio running FAST (further into the media than the picture); <1.00 =
    // audio running slow. Only meaningful while contiguous in the same run
    // (reset on a re-anchor so before/after scrub speed is cleanly separated).
    double speed_ratio = 0.0;
    if (speed_run_ == run_id_ && speed_video_ms_ > 0.0) {
        const double dv = video_ms - speed_video_ms_;
        const double da = audible_ms - speed_audible_ms_;
        if (dv > 1.0 && da >= 0.0) speed_ratio = da / dv;
    }
    speed_run_ = run_id_;
    speed_video_ms_ = video_ms;
    speed_audible_ms_ = audible_ms;

    ::canvas::core::log::log_warning(
        "[avsync] run=%llu frame=%lld video_ms=%.1f audible_ms=%.1f av_offset_ms=%.1f "
        "speed_x=%.3f write_to_audible_ms=%.1f step_s=%.4f",
        static_cast<unsigned long long>(run_id_), static_cast<long long>(seq_frame), video_ms,
        audible_ms, video_ms - audible_ms, speed_ratio, written_ms - audible_ms, step_seconds);
}

void AudioPipeline::play_step(int64_t seq_frame, double step_seconds, bool seek_hold_active) {
    std::lock_guard lock(mutex_);
    // Throttle failure warnings to ~1/second since this runs every frame.
    static auto last_warn = Clock::now();
    const auto warn = [&](const char* why) {
        const auto now = Clock::now();
        if (now - last_warn < std::chrono::seconds(1)) return;
        last_warn = now;
        ::canvas::core::log::log_warning("audio: no output at frame %lld -> %s",
                                     static_cast<long long>(seq_frame), why);
    };

    if (!project_ || !active_ || !sink_.is_open()) {
        warn("not playing / output closed");
        return;
    }
    // SonicSync seek-hold: while a seek-while-playing is still decoding/presenting
    // its target frame, do not feed per-frame audio for the (old) playhead — it
    // would stream ahead of the frozen picture. handle_seek closes the hold at the
    // atomic re-anchor (frame presented) and re-anchors audio to the target itself.
    if (seek_hold_active) {
        warn("seek-hold active (audio waits for its frame)");
        return;
    }
    // Suppressed during a playing scrub? No — for the Premiere-style model the
    // forward program audio IS the scrub audio: the drag streams play_step from
    // whatever position the playhead is at (see the playing branch in
    // handle_seek_preview), so this must NOT early-return while scrubbing. The
    // old MLT-blip feed (feed_scrub_audio) is gone.
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) {
        // Ghost-audio diagnostic: distinguish "clip present but DISABLED" from
        // "no clip at all". If a disabled clip is hit here while the user still
        // hears audio, the pipeline's project is stale (disable never landed).
        const canvas::core::Clip* present = clip_at_any_track(seq_frame);
        if (present && present->media >= 0) {
            // Fires every frame while playing over a disabled clip (~60/s of
            // identical lines) or video-only media with no audio track. Throttle
            // to ~1/s with a suppressed counter so the log stays readable while
            // still proving how often the guard fired.
            static auto last_gg = Clock::now();
            static int gg_ = 0, gg_suppressed_ = 0;
            if ((++gg_) == 1 || Clock::now() - last_gg >= std::chrono::seconds(1)) {
                last_gg = Clock::now();
                if (gg_suppressed_)
                    ::canvas::core::log::log_warning(
                        "audio: GHOST-GUARD frame=%lld enabled=%d media=%d tl=%lld->%lld suppressed=%d",
                        static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                        static_cast<long long>(present->tl_in),
                        static_cast<long long>(present->tl_out), gg_suppressed_);
                else
                    ::canvas::core::log::log_warning(
                        "audio: GHOST-GUARD frame=%lld enabled=%d media=%d tl=%lld->%lld",
                        static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                        static_cast<long long>(present->tl_in),
                        static_cast<long long>(present->tl_out));
                gg_suppressed_ = 0;
            } else {
                ++gg_suppressed_;
            }
        }
        warn(present && present->media >= 0 ? "ghost-guard: clip present but (locked/disabled)"
                                            : "no audio clip at playhead");
        return;
    }

    auto ait = decoders_.find(clip->media);
    if (ait == decoders_.end() || !ait->second->has_audio()) {
        warn("no audio decoder for clip");
        return;
    }

    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) {
        warn("media fps unknown for audio clip");
        return;
    }

    const int64_t src_frame = clip->src_in + (seq_frame - clip->tl_in);
    const int64_t start_sample =
        static_cast<int64_t>(std::llround(src_frame / fps_v * rate_));
    const int64_t want = std::max<int64_t>(1, static_cast<int64_t>(std::llround(step_seconds * rate_)));
    // Always-ON A/V sync (throttled to ~1/s internally). Uses the device's true
    // audible position anchored to this run, so default captures show the real
    // audio-vs-video offset and let us compare run N (before scrub) vs N+1.
    log_av_sync(seq_frame, fps_v, step_seconds);

    // Skip any leading media samples this play run already fed to the device
    // (from preroll's pre-roll), so a present never re-writes pre-rolled audio
    // and drives the audible cursor ahead of the picture. The feed watermark
    // lives in this clip's audio domain (per media) and is reset each rewind; it
    // is ignored if the playhead has crossed to another clip.
    const int64_t from =
        clip->media == feed_media_ ? std::max(start_sample, feed_sample_) : start_sample;
    const int64_t span = start_sample + want - from;
    feed_media_ = clip->media;
    feed_sample_ = std::max(feed_sample_, start_sample + want);
    if (span <= 0) {
        if (playback_dbg())
            ::canvas::core::log::log_warning(
                "audio: play step SKIPPED span<=0 seq_frame=%lld start_sample=%lld from=%lld "
                "want=%lld feed_sample=%lld",
                static_cast<long long>(seq_frame), static_cast<long long>(start_sample),
                static_cast<long long>(from), static_cast<long long>(want),
                static_cast<long long>(feed_sample_));
        return;
    }

    auto chunk = ait->second->decode(from, static_cast<int>(std::min<int64_t>(span, want)), rate_);
    if (chunk && !chunk->samples.empty()) {
        const int frames = static_cast<int>(chunk->samples.size()) / chunk->channels;
        std::vector<float> fade_scratch;
        const std::vector<float>* data =
            apply_audio_fades(*clip, chunk, from, rate_, fps_v, &fade_scratch);
        if (playback_dbg()) {
            // Diagnostic: report the actual signal level of what we hand to the
            // output device so "no sound" can be told apart from "silence". A
            // chunk full of near-zero samples reaching a RUNNING ALSA device
            // means the decode produced silence, not a delivery problem.
            float peak = 0.0f, sum_sq = 0.0f;
            int nonzero = 0;
            for (const float s : chunk->samples) {
                const float a = s < 0.0f ? -s : s;
                if (a > peak) peak = a;
                sum_sq += s * s;
                if (a > 1e-5f) ++nonzero;
            }
            const float rms = static_cast<float>(std::sqrt(sum_sq / chunk->samples.size()));
            const bool ok = sink_.write_float(data->data(), frames);
            ::canvas::core::log::log_warning(
                "audio: play step frame=%lld start_sample=%lld out_frames=%d write_float_ok=%d "
                "peak=%.5f rms=%.5f nonzero=%d/%zu ch=%d sr=%d",
                static_cast<long long>(seq_frame), static_cast<long long>(start_sample), frames,
                (int)ok, peak, rms, nonzero, chunk->samples.size(), chunk->channels,
                chunk->sample_rate);
        } else {
            const bool ok = sink_.write_float(data->data(), frames);
            (void)ok;
        }
    } else {
        warn("decode produced no samples");
    }

    // ALWAYS-ON audio feed health (~1/s via log_warning): what media position we
    // just handed to the device, how many frames, and the offset between the
    // media time we're WRITING now vs the video frame being presented now.
    // churn / gaps here are what the listener hears as audio artifacts.
    static auto last_afe_log = Clock::now();
    static int afe_ = 0;
    if ((++afe_) == 1 || Clock::now() - last_afe_log >= std::chrono::seconds(1)) {
        last_afe_log = Clock::now();
        static int64_t last_start = 0;
        // Samples actually handed to the device on this call (NOT the seek-jump
        // media delta): a value far above the ~per-frame `want` means a burst.
        const int64_t written_now =
            chunk ? static_cast<int64_t>(chunk->samples.size()) /
                        (chunk->channels ? chunk->channels : 1)
                  : 0;
        const bool seek_hop = last_start != 0 && std::llabs(from - last_start) > want * 4;
        const bool burst = written_now > want * 4;
        last_start = from;
        // True audible-vs-picture offset via the device's measured audible position
        // (written minus device buffer) ANCHORED to this run, so the reported
        // audible media-time matches the timeline the way the [avsync] line does.
        // (The raw device counter is cumulative across all runs since open, so
        // using it directly would be misleading after any seek/scrub.)
        const uint64_t audible_frames = sink_.audible_position_frames();
        const int64_t run_audible =
            static_cast<int64_t>(audible_frames) - static_cast<int64_t>(written_at_anchor_);
        const double audible_ms =
            static_cast<double>(anchor_media_sample_ + run_audible) / static_cast<double>(rate_) * 1000.0;
        const double video_ms = static_cast<double>(src_frame) / fps_v * 1000.0;
        ::canvas::core::log::log_warning(
            "[audio] frame=%lld wrote=%lld req=%lld seek_hop=%d burst=%d audible_ms=%.1f "
            "video_ms=%.1f av_offset_ms=%.1f churn=%d",
            static_cast<long long>(seq_frame), static_cast<long long>(written_now),
            static_cast<long long>(want), (int)seek_hop, (int)burst, audible_ms, video_ms,
            audible_ms - video_ms, (int)(chunk && chunk->samples.empty()));
    }
}

// Playing-scrub audio feed (MLT model). Called from the UI thread by the
// controller's seek_preview() on each mouse move while a playback drag is active
// (the worker is saturated decoding video previews during a drag and cannot feed
// in time). Each landed position is reposition_enqueue()'d: the device is DROPPED
// + re-prepared and the new position's chunk becomes the next thing the device
// plays. This makes the AUDIBLE position jump with the drag (an enqueue-only FIFO
// feed can never advance the audible faster than realtime). No thread stop/join,
// no teardown gaps; the writer discards stale in-flight batches via the
// generation counter. Throttled to ~45ms so the audible "ladder" tracks the drag
// without device chop.
void AudioPipeline::feed_scrub_audio(int64_t target) {
    // Throttle to ~every 45ms: each reposition gives the device ~45ms of the new
    // position to actually play before the next reposition replaces it. A shorter
    // gap chops every blip into inaudible fragments; a longer one makes the ladder
    // feel sparse. Chunk is 120ms so the device never STARVES between repositions.
    const auto now = Clock::now();
    if (target == last_scrub_audio_target_ ||
        now - last_scrub_audio_at_ < std::chrono::milliseconds(45))
        return;
    last_scrub_audio_at_ = now;

    std::lock_guard lock(mutex_);
    if (!project_ || !active_ || !sink_.is_open()) return;
    const canvas::core::Clip* clip = audio_clip_at(target);
    if (!clip || clip->media < 0) return;
    auto ait = decoders_.find(clip->media);
    if (ait == decoders_.end() || !ait->second->has_audio()) return;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return;
    const int64_t src_frame = clip->src_in + (target - clip->tl_in);
    const int64_t base_sample =
        static_cast<int64_t>(std::llround(src_frame / fps_v * rate_));
    // AUDIBLE-following reposition: each landed scrub position supplies a ~120ms
    // chunk and we DROP the device + repoint it at this chunk, so the audible
    // position jumps with the drag instead of only advancing at realtime through
    // a FIFO (the enqueue-only feed never moved audibly). The next reposition
    // (~45ms later) drops and replaces this chunk, so during a fast drag you hear
    // the classic positional-blip ladder; on a slow drag each position gets a
    // full audible blip. No writer stop/join — the writer discards stale batches
    // via the generation counter.
    const int64_t chunk_frames =
        static_cast<int64_t>(static_cast<double>(120) / 1000.0 * rate_);
    auto s = ait->second->decode(base_sample, static_cast<int>(chunk_frames), rate_);
    if (!s || s->samples.empty()) return;
    const int frames = static_cast<int>(s->samples.size()) / s->channels;
    if (frames <= 0) return;
    const std::size_t pending_before = sink_.pending_frames();
    bool ok = false;
    if (sink_.is_open()) {
        std::vector<float> fade_scratch;
        const std::vector<float>* data =
            apply_audio_fades(*clip, s, base_sample, rate_, fps_v, &fade_scratch);
        ok = sink_.reposition_enqueue(data->data(), frames);
    }
    const std::size_t pending_after = sink_.pending_frames();
    if (ok) ++scrub_repositions_;
    last_scrub_audio_target_ = target;
    const double target_ms = static_cast<double>(target) / fps_v * 1000.0;
    const double chunk_ms = static_cast<double>(frames) / rate_ * 1000.0;
    ::canvas::core::log::log_warning(
        "[scrub] FEED target=%lld target_ms=%.0f base_sample=%lld chunk_ms=%.0f frames=%d "
        "pending_before=%zu pending_after=%zu%s",
        static_cast<long long>(target), target_ms, static_cast<long long>(base_sample), chunk_ms,
        frames, pending_before, pending_after, ok ? "" : " DROPPED");
}

void AudioPipeline::begin_scrub() {
    std::lock_guard lock(mutex_);
    // Fresh drag: forget the previous drag's scrub-audio feed state so the first
    // move always feeds.
    last_scrub_audio_target_ = -1;
    last_scrub_audio_at_ = {};
    scrub_repositions_ = 0;
}

int64_t AudioPipeline::audible_seq_frame(int64_t playhead_seq) const {
    std::lock_guard lock(mutex_);
    // Without a live, ENABLED audio clip under the playhead nothing is audible,
    // so there is no master clock to trust this run (see present_next notes).
    if (!active_ || !audio_clip_at(playhead_seq)) return -1;
    const uint64_t aud_frames = sink_.audible_position_frames();
    const int64_t run_aud =
        static_cast<int64_t>(aud_frames) - static_cast<int64_t>(written_at_anchor_);
    const int64_t aud_sample = anchor_media_sample_ + run_aud;
    if (aud_sample < 0) return -1;
    return audio_sample_to_seq_frame(aud_sample);
}

void AudioPipeline::advance_feed_for_drop(int64_t new_frame) {
    std::lock_guard lock(mutex_);
    if (!active_ || feed_media_ < 0) return;
    const canvas::core::Clip* ac = audio_clip_at(new_frame);
    if (!ac || ac->media != feed_media_) return;
    const double afps = media_fps_of(*project_, *ac);
    if (afps > 0.0) {
        const int64_t asrc = ac->src_in + (new_frame - ac->tl_in);
        feed_sample_ = std::max(
            feed_sample_, static_cast<int64_t>(std::llround(asrc / afps * rate_)));
    }
}

canvas::core::MediaId AudioPipeline::audio_media_at(int64_t seq_frame) const {
    std::lock_guard lock(mutex_);
    const canvas::core::Clip* c = audio_clip_at(seq_frame);
    return c ? c->media : -1;
}

}  // namespace canvas::gui