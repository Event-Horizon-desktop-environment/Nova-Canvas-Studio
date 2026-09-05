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

// Applies a clip's IN/OUT transition envelopes to decoded PCM before it
    // reaches the device, matching the exporter's audio mix. Returns the chunk
    // itself when no fade touches this range, else a gain-scaled copy in scratch.
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
        // Floor, not llround: the gains land on the same whole frames the
        // renderer's audio_chunk uses (llround shifts frame boundaries).
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
    // New run: the sync log compares A/V offset before vs after the re-anchor.
    ++run_id_;
    ::canvas::core::log::log_warning("[avsync] RE-ANCHOR run=%llu at_seq_frame=%lld device_written=%llu",
                                 static_cast<unsigned long long>(run_id_),
                                 static_cast<long long>(seq_frame),
                                 static_cast<unsigned long long>(sink_.stat_written_frames()));
    // Remember anchor media sample + device-written counter so the audible
    // position within this run can be derived for A/V sync diagnostics.
    written_at_anchor_ = sink_.stat_written_frames();
    anchor_media_sample_ = playhead_to_audio_sample(seq_frame);
    // Fresh run: nothing fed yet, so the feed watermark starts empty and the
    // next preroll/play_step starts at the anchor (no double-handoff).
    feed_sample_ = -1;
    feed_media_ = -1;
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: rewind reset feed, anchor_media_sample=%lld rate=%d",
                                     static_cast<long long>(anchor_media_sample_), rate_);
    for (auto& [id, adec] : decoders_) adec->reset();
    // Re-arm the device on every seek so playback restarts from the new
    // playhead; without this, resume would play stale audio from the old spot.
    if (active_) {
        sink_.log_pipeline_stats("rewind-pre");
        sink_.flush();
        sink_.log_pipeline_stats("rewind-post");
    }
}

// Write `lead_ms` of audio into the output before the first video frame is
// presented. The device holds fixed buffer latency (~70ms observed), so audio
// written when a frame shows is HEARD ~70ms later. Pre-filling with leading
// audio puts the audible cursor (~written minus latency) on the picture.
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
    // Pre-roll writes the first `total` media samples at the anchor. Advance the
    // feed watermark so play_step() continues after this lead-in instead of
    // re-writing it (which pulls the audible cursor ahead of video).
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

// Audible scrub: decode a short (~40ms) PCM grain at `seq_frame` and write it
// to the already-open device, keeping each grain distinct and cheap. Best-effort;
// mirrors preroll's decode+write pattern for a discrete scrub blip. The device
// stays open across the drag so no per-move open/flush (the reported stutter).
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
    // An audio-track clip covering the playhead decides this frame's output: if
    // enabled it plays, if disabled it's an intentional mute. Either way do NOT
    // fall through to the video track — that would let a disabled audio clip be
    // overridden by the movie's embedded audio (breaking Ctrl+D mute).
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

// Media-time audio sample index for a timeline frame. Matches play_step()'s
    // computation so diagnostics and playback agree.
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
    // Find the audio clip whose [src_in, src_out) covers this sample and map it
    // back to its timeline placement. Audio tracks are few; a linear scan on
    // drop-ahead (once per slow present) is fine.
    //
    // Inverts playhead_to_audio_sample(): first convert the sample back to an
    // absolute media-frame position (src_frame) that already includes src_in,
    // then subtract src_in before adding tl_in. The old version compared the
    // sample-domain value directly against src_in (frame domain, unit mismatch)
    // and added src_in twice, so trimmed clips inflated the audible frame fed to
    // SonicSync's master-clock cap — the observed forward-scrub desync.
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

// Debug output for A/V sync, logged ~1/s: the frame being presented now versus
// the audio position currently AUDIBLE (anchored to the last rewind so seeks/
// scrubs don't corrupt the comparison).
//
//   video_ms     = media time of the picture on screen
//   audible_ms   = media time of the sound at the speaker
//   av_offset_ms = video_ms - audible_ms  (>0: audio heard is BEHIND the picture)
//
// Steady play: a small, roughly constant positive offset (~buffer latency) is
// expected. Offsets that GROW mean real drift; a jump right after a scrub means
// audio wasn't re-anchored to the new playhead.
//
// NOTE: does NOT lock mutex_ — the caller (play_step) already holds it, and
// taking it here deadlocks the worker on the first frame.
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

    // AUDIO SPEED within this run: audible-media-time advance vs video-media-time
    // over the last ~1s. 1.00 = correct pace; >1.00 = audio running fast. Only
    // meaningful while contiguous in one run (reset on re-anchor).
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
    // Throttle failure warnings to ~1/s since this runs every frame.
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
    // SonicSync seek-hold: while a seek-while-playing is still decoding its target,
    // do not feed audio for the (old) playhead — it would stream ahead of the
    // frozen picture. handle_seek closes the hold at the re-anchor and re-anchors
    // audio to the target itself.
    if (seek_hold_active) {
        warn("seek-hold active (audio waits for its frame)");
        return;
    }
    // Not suppressed during a playing scrub: the drag streams play_step from
    // wherever the playhead is (see the playing branch in handle_seek_preview),
    // so the forward program audio IS the scrub audio. The old blip feed is gone.
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) {
        // Ghost-audio diagnostic: tell a clip that's present but DISABLED apart from
    // no clip at all. Hitting this while the user still hears audio means the
    // pipeline's project is stale (disable never landed).
        const canvas::core::Clip* present = clip_at_any_track(seq_frame);
        if (present && present->media >= 0) {
            // Fires every frame while playing over a disabled clip or video-only
            // media. Throttle to ~1/s so the log stays readable while still
            // proving how often the guard fired.
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

    // Skip leading media samples this run already fed to the device (from preroll),
    // so a present never re-writes pre-rolled audio and pulls the audible cursor
    // ahead of the picture. The watermark lives per media, is reset on rewind,
    // and is ignored once the playhead crosses to another clip.
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
            // Diagnostic: actual signal level of what reaches the device, so "no sound"
            // can be told apart from "silence". Near-zero samples into a RUNNING
            // device means the decode produced silence, not a delivery problem.
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

    // Always-on audio feed health (~1/s): media position just handed to the device,
    // how many frames, and the offset vs the frame being presented. Churn/gaps
    // are what the listener hears as artifacts.
    static auto last_afe_log = Clock::now();
    static int afe_ = 0;
    if ((++afe_) == 1 || Clock::now() - last_afe_log >= std::chrono::seconds(1)) {
        last_afe_log = Clock::now();
        static int64_t last_start = 0;
        // Samples handled to the device on this call (not the seek-jump media delta):
        // a value far above the per-frame `want` means a burst.
        const int64_t written_now =
            chunk ? static_cast<int64_t>(chunk->samples.size()) /
                        (chunk->channels ? chunk->channels : 1)
                  : 0;
        const bool seek_hop = last_start != 0 && std::llabs(from - last_start) > want * 4;
        const bool burst = written_now > want * 4;
        last_start = from;
        // True audible-vs-picture offset via the device's measured audible position
        // (written minus device buffer), anchored to this run so it matches the
        // timeline the way the [avsync] line does. (The raw device counter spans
        // all runs since open, so using it directly misleads after any seek.)
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

// Playing-scrub audio feed (MLT model), called from the UI thread by the
// controller's seek_preview() on each mouse move while a playback drag is active
// (the worker is busy decoding previews and can't feed in time). Each landed
// position is reposition_enqueue()'d: the device is dropped + re-prepared and the
// new chunk becomes the next thing it plays, so the AUDIBLE position jumps with
// the drag (an enqueue-only FIFO can never advance audibly faster than realtime).
// No thread stop/join, no teardown gaps; the writer drops stale batches via the
// generation counter. Throttled to ~45ms so the audible "ladder" tracks the drag
// without device chop.
void AudioPipeline::feed_scrub_audio(int64_t target) {
    // Throttle to ~45ms: each reposition gives the device ~45ms of the new
    // position before the next one replaces it. Shorter chops every blip into
    // inaudible fragments; longer makes the ladder feel sparse. Chunk is 120ms
    // so the device never starves between repositions.
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
    // chunk and the device is dropped + repointed at it, so the audible position
    // jumps with the drag. The next reposition (~45ms later) replaces it, so a
    // fast drag gives the classic blip ladder and a slow drag full audible blips.
    // No writer stop/join — the generation counter drops any stale batch.
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
    // Fresh drag: forget the previous drag's scrub-audio state so the first move always feeds.
    last_scrub_audio_target_ = -1;
    last_scrub_audio_at_ = {};
    scrub_repositions_ = 0;
}

int64_t AudioPipeline::audible_seq_frame(int64_t playhead_seq) const {
    std::lock_guard lock(mutex_);
    // Without a live, ENABLED audio clip under the playhead nothing is audible, so
    // there is no master clock to trust this run (see present_next notes).
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