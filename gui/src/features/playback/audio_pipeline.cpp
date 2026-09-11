#include "audio_pipeline.hpp"

#include "audio_sink.hpp"
#include "sync_constants.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

// Patch the RIFF/data sizes of a float-PCM WAV started by
    // maybe_wave_capture_open_locked() once the run is over.
    void patch_wav_header(std::FILE* f, std::size_t total_frames, int channels) {
        if (!f) return;
        const std::uint32_t data_bytes =
            static_cast<std::uint32_t>(static_cast<std::size_t>(total_frames) *
                                       static_cast<std::size_t>(channels) * 4u);
        const std::uint32_t riff = 36u + data_bytes;
        std::fseek(f, 4, SEEK_SET);
        std::fwrite(&riff, 4, 1, f);
        std::fseek(f, 40, SEEK_SET);
        std::fwrite(&data_bytes, 4, 1, f);
        std::fflush(f);
    }

double media_fps_of(const canvas::core::Project& project, const canvas::core::Clip& clip) {
    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                 [&](const canvas::core::MediaEntry& m) { return m.id == clip.media; });
    return (it != project.media.end() && it->fps > 0.0) ? it->fps : 0.0;
}

// Per-source fade envelope for a decoded chunk: one gain per output frame from
// the clip's IN/OUT audio transitions (1.0 everywhere when no audio fade touches
// this span). Leaves `out` empty for the unity case so callers pass nullptr and
// skip the per-sample multiply entirely.
void compute_fade_gains(const canvas::core::Clip& clip, const int64_t from_sample,
                        const int out_rate, const double src_fps, const double seq_fps,
                        const int frames, std::vector<float>* out) {
    const bool has_fade =
        (clip.transition_in_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_in)) ||
        (clip.transition_out_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_out));
    if (!has_fade) return;
    // Fade envelopes live in SEQUENCE time: samples into the clip's trimmed
    // audio map back to timeline frames at the sequence rate, with the clip
    // start positioned by the source's own media fps. Mixed frame-rate clips
    // therefore fade in the same realtime position write_mixed() voices them at.
    const double base_sample = static_cast<double>(clip.src_in) / src_fps * out_rate;
    out->resize(static_cast<std::size_t>(frames));
    for (int k = 0; k < frames; ++k) {
        // Floor, not llround: the gains land on the same whole frames the
        // renderer's audio_chunk uses (llround shifts frame boundaries).
        const int64_t tl = clip.tl_in + static_cast<int64_t>(std::floor(
            (static_cast<double>(from_sample + k) - base_sample) / out_rate * seq_fps));
        (*out)[static_cast<std::size_t>(k)] = canvas::core::audio_fade_gain(clip, tl);
    }
}

// Decodes the source chunk's per-frame fade envelope + clip Volume/Pan (balance)
// + track gain into `out` (frames x out_channels). Uses the shared
// audio_mix::mix_chunk law so playback/export/scrub mix identically: mono is
// upmixed to the front pair, >2ch sources fold down for stereo buses or map
// channel-for-channel for wider ones.
void mix_source_chunk(std::vector<float>& out, const int out_channels,
                      const canvas::core::Clip& clip, const float track_gain_db,
                      const canvas::core::AudioChunkPtr& chunk, const int64_t from_sample,
                      const int out_rate, const double src_fps, const double seq_fps) {
    const int src_ch = chunk->channels > 0 ? chunk->channels : 1;
    const int frames = static_cast<int>(chunk->samples.size()) / src_ch;
    if (frames <= 0) return;
    std::vector<float> gains;
    compute_fade_gains(clip, from_sample, out_rate, src_fps, seq_fps, frames, &gains);
    float gl = 1.0f, gr = 1.0f;
    canvas::core::audio_mix::pan_gains(clip.pan, gl, gr);
    const float vol = canvas::core::audio_mix::db_to_gain(clip.volume_db) *
                      canvas::core::audio_mix::db_to_gain(track_gain_db);
    canvas::core::audio_mix::mix_chunk(out, out_channels, chunk->samples, src_ch,
                                       gains.empty() ? nullptr : &gains, vol, gl, gr);
}
}  // namespace

AudioPipeline::~AudioPipeline() { reset(); }

void AudioPipeline::set_project(const canvas::core::Project* project) {
    std::lock_guard lock(mutex_);
    project_ = project;
}

void AudioPipeline::update_project(const canvas::core::Project* project) {
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
            ::canvas::core::log::log_audio_warning(
                "audio: FAILED to open audio decoder for media %d path=%s",
                entry.id, entry.path.c_str());
        else
            ::canvas::core::log::log_audio_warning(
                "audio: media %d has no audio stream (or open failed) path=%s",
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
    feed_watermarks_.clear();
    last_seq_fed_ = -1;
    last_scrub_audio_target_ = -1;
    last_scrub_audio_at_ = {};
    scrub_repositions_ = 0;
    wave_capture_close_locked();
    feed_ledger_frames_ = 0;
    feed_ledger_at_ = {};
    last_primary_clip_ = -1;
    cut_diag_ = 0;
}

void AudioPipeline::open_output() {
    std::lock_guard lock(mutex_);
    if (active_) return;
    active_ = sink_.open(rate_, channels_);
    if (!active_)
<<<<<<< Updated upstream
        ::canvas::core::log::log_warning("audio: FAILED to open output device rate=%d channels=%d",
                                     rate_, channels_);
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: pipeline open output active=%d rate=%d channels=%d",
                                     (int)active_, rate_, channels_);
=======
        ::canvas::core::log::log_audio_warning(
            "audio: FAILED to open output device rate=%d channels=%d", rate_, channels_);
    CANVAS_LOG("audio: pipeline open output active=%d rate=%d channels=%d", (int)active_, rate_,
               channels_);
>>>>>>> Stashed changes
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
    reanchor_locked(seq_frame);
}

// Re-arm the output at `seq_frame` assuming mutex_ is already held. Bumps the
// run id, re-anchors the audible-position bookkeeping (device-written counter +
// media sample so A/V sync math restarts here), clears the feed watermarks so
// the next mix starts exactly at the new playhead, resets every decoder to the
// stream start, and flushes the device so it can never keep playing the
// previous run's audio (stale-audio-ahead garbling). `seq_frame` is also
// recorded as the last-fed frame so play_step()'s discontinuity check starts
// clean.
void AudioPipeline::reanchor_locked(int64_t seq_frame) {
    // New run: the sync log compares A/V offset before vs after the re-anchor.
    ++run_id_;
<<<<<<< Updated upstream
    ::canvas::core::log::log_warning("[avsync] RE-ANCHOR run=%llu at_seq_frame=%lld device_written=%llu",
                                 static_cast<unsigned long long>(run_id_),
                                 static_cast<long long>(seq_frame),
                                 static_cast<unsigned long long>(sink_.stat_written_frames()));
=======
    ::canvas::core::log::log_audio_info(
        "[avsync] RE-ANCHOR run=%llu at_seq_frame=%lld device_written=%llu",
        static_cast<unsigned long long>(run_id_), static_cast<long long>(seq_frame),
        static_cast<unsigned long long>(sink_.stat_written_frames()));
>>>>>>> Stashed changes
    // Remember anchor media sample + device-written counter so the audible
    // position within this run can be derived for A/V sync diagnostics.
    written_at_anchor_ = sink_.stat_written_frames();
    anchor_media_sample_ = playhead_to_audio_sample(seq_frame);
    anchor_seq_frame_ = seq_frame;
    // Fresh run: nothing fed yet, so the feed watermarks start empty and the
    // next preroll/play_step starts at the anchor (no double-handoff).
    feed_watermarks_.clear();
    last_seq_fed_ = seq_frame;
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
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty()) return;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return;
    const int64_t total = static_cast<int64_t>(static_cast<double>(lead_ms) / 1000.0 * rate_);
    // Pre-roll writes the first `total` media samples at the anchor as a MIX of
    // every audible source. The per-media feed watermarks advance inside
    // write_mixed, so play_step() continues after each lead-in instead of
    // re-writing it (which pulls the audible cursor ahead of video).
    const int64_t written = write_mixed(seq_frame, total);
<<<<<<< Updated upstream
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: preroll lead_ms=%d written_frames=%lld sources=%zu",
                                     lead_ms, static_cast<long long>(written), sources.size());
    ::canvas::core::log::log_warning(
=======
    CANVAS_LOG("audio: preroll lead_ms=%d written_frames=%lld sources=%zu", lead_ms,
               static_cast<long long>(written), sources.size());
    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
        "[audio:preroll] cur=%lld base_sample=%lld written_frames=%lld audible_before=%llu playing=%d",
        static_cast<long long>(seq_frame), static_cast<long long>(playhead_to_audio_sample(seq_frame)),
        static_cast<long long>(written),
        static_cast<unsigned long long>(sink_.audible_position_frames()), (int)playing);
}

// === diagnostic wave capture + feed ledger ===
// set_wave_capture(path) enables/disables the device-bound mix capture; with an
// empty path it also suppresses the CANVAS_DEBUG_CAPTURE_WAV env fallback.
void AudioPipeline::set_wave_capture(const char* path) {
    std::lock_guard lock(mutex_);
    wave_capture_close_locked();
    if (path && *path) {
        wav_capture_path_ = path;
    }
    wav_capture_disabled_ = !path || !*path;
    maybe_wave_capture_open_locked();
}

void AudioPipeline::close_wave_capture() {
    std::lock_guard lock(mutex_);
    wave_capture_close_locked();
}

// Open the capture lazily right before the first write so the pipeline's final
// rate/channels are known. Source = the explicit set_wave_capture() path, else
// the CANVAS_DEBUG_CAPTURE_WAV environment variable.
void AudioPipeline::maybe_wave_capture_open_locked() {
    if (wav_capture_f_) return;
    if (wav_capture_disabled_) return;
    const char* path = wav_capture_path_.empty()
                           ? std::getenv("CANVAS_DEBUG_CAPTURE_WAV")
                           : wav_capture_path_.c_str();
    if (!path || !*path) return;
    wav_capture_f_ = std::fopen(path, "wb");
    if (!wav_capture_f_) {
        ::canvas::core::log::log_audio_warning("audio: wave capture open FAILED path=%s", path);
        return;
    }
    wav_capture_frames_ = 0;
    // 44-byte canonical header for float32 PCM; RIFF/data sizes patched on close.
    const uint32_t sr = static_cast<uint32_t>(rate_);
    const uint32_t byte_rate = sr * static_cast<uint32_t>(channels_) * 4u;
    const uint16_t block_align = static_cast<uint16_t>(channels_ * 4);
    std::fwrite("RIFF", 1, 4, wav_capture_f_);
    const uint32_t riff_tmp = 0;
    std::fwrite(&riff_tmp, 4, 1, wav_capture_f_);
    std::fwrite("WAVEfmt ", 1, 8, wav_capture_f_);
    const uint32_t fmt_sz = 16;
    std::fwrite(&fmt_sz, 4, 1, wav_capture_f_);
    const uint16_t fmt_id = 3;  // IEEE float
    std::fwrite(&fmt_id, 2, 1, wav_capture_f_);
    const uint16_t nch = static_cast<uint16_t>(channels_);
    std::fwrite(&nch, 2, 1, wav_capture_f_);
    std::fwrite(&sr, 4, 1, wav_capture_f_);
    std::fwrite(&byte_rate, 4, 1, wav_capture_f_);
    std::fwrite(&block_align, 2, 1, wav_capture_f_);
    const uint16_t bits = 32;
    std::fwrite(&bits, 2, 1, wav_capture_f_);
    std::fwrite("data", 1, 4, wav_capture_f_);
    const uint32_t data_tmp = 0;
    std::fwrite(&data_tmp, 4, 1, wav_capture_f_);
    if (::canvas::core::log::enabled())
        ::canvas::core::log::log_warning("audio: wave capture ON -> %s rate=%d ch=%d", path,
                                     rate_, channels_);
}

void AudioPipeline::wave_capture_write_locked(const float* data, std::size_t frames) {
    if (!wav_capture_f_) return;
    std::fwrite(data, sizeof(float), frames * static_cast<std::size_t>(channels_), wav_capture_f_);
    wav_capture_frames_ += frames;
}

void AudioPipeline::wave_capture_close_locked() {
    if (!wav_capture_f_) return;
    patch_wav_header(wav_capture_f_, static_cast<std::size_t>(wav_capture_frames_), channels_);
    std::fclose(wav_capture_f_);
    wav_capture_f_ = nullptr;
<<<<<<< Updated upstream
    ::canvas::core::log::log_warning(
=======
    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
        "audio: wave capture OFF frames=%llu path=%s",
        static_cast<unsigned long long>(wav_capture_frames_), wav_capture_path_.c_str());
}

// ~1Hz feed ledger: how many frames the mix handed to the sink this window vs
// how many the device actually advanced, plus the sink's pending buffer. A burst
// (fed >> device) followed by silence is the fingerprint of queue-overflow
// tearing; fed == device implies clean steady-state pacing.
void AudioPipeline::log_feed_ledger_locked(int64_t seq_frame, int64_t start_sample, int64_t from,
                                           int64_t want, int64_t written) {
    feed_ledger_frames_ += static_cast<uint64_t>(written > 0 ? written : 0);
    const auto now = Clock::now();
    if (feed_ledger_at_.time_since_epoch().count() == 0) {
        feed_ledger_at_ = now;
        return;
    }
    if (now - feed_ledger_at_ < std::chrono::seconds(1)) return;
    const double secs = std::chrono::duration<double>(now - feed_ledger_at_).count();
    feed_ledger_at_ = now;
    static uint64_t last_dev_written = 0;
    const uint64_t dev_written = sink_.stat_written_frames();
    const int64_t dev_delta =
        static_cast<int64_t>(dev_written) - static_cast<int64_t>(last_dev_written);
    last_dev_written = dev_written;
<<<<<<< Updated upstream
    ::canvas::core::log::log_warning(
=======
    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
        "[audio:feed] frame=%lld start=%lld from=%lld want=%lld wrote=%lld fed_window=%llu "
        "dev_written_delta=%lld (%.0f/s) pending=%zu (%.0fms) audible=%llu dev_lat_ms=%.1f",
        static_cast<long long>(seq_frame), static_cast<long long>(start_sample),
        static_cast<long long>(from), static_cast<long long>(want), static_cast<long long>(written),
        static_cast<unsigned long long>(feed_ledger_frames_), static_cast<long long>(dev_delta),
        3600.0, sink_.pending_frames(),
        static_cast<double>(sink_.pending_frames()) / static_cast<double>(rate_) * 1000.0,
        static_cast<unsigned long long>(sink_.audible_position_frames()),
        static_cast<double>(static_cast<int64_t>(dev_written) -
                            static_cast<int64_t>(sink_.audible_position_frames())) /
            static_cast<double>(rate_) * 1000.0);
    feed_ledger_frames_ = 0;
    sink_.log_pipeline_stats("feed");
}

// Audible scrub: decode a short (~40ms) PCM grain at `seq_frame` and write it
// to the already-open device, keeping each grain distinct and cheap. Best-effort;
// mirrors preroll's decode+write pattern for a discrete scrub blip. The device
// stays open across the drag so no per-move open/flush (the reported stutter).
void AudioPipeline::play_scrub_grain(int64_t seq_frame) {
    std::lock_guard lock(mutex_);
    if (!project_ || !active_ || !sink_.is_open()) return;
    const auto sources = audible_sources_at(seq_frame);
    const canvas::core::Clip* clip = sources.empty() ? nullptr : sources[0].clip;
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
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return;
    const int64_t base_sample = playhead_to_audio_sample(seq_frame);
    const int64_t grain_frames =
        static_cast<int64_t>(static_cast<double>(40) / 1000.0 * rate_);  // ~40ms
    const auto g0 = Clock::now();
    auto s = ait->second->decode(base_sample, static_cast<int>(grain_frames), rate_);
    const double dec_ms = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
    if (!s || s->samples.empty()) return;
    const int f = static_cast<int>(s->samples.size() / s->channels);
    if (f <= 0) return;
    std::vector<float> grain(static_cast<std::size_t>(f) * channels_, 0.0f);
    mix_source_chunk(grain, channels_, *clip, sources[0].gain_db, s, base_sample, rate_, fps_v,
                     seq_fps);
    if (!sink_.write_float(grain.data(), f))
        ::canvas::core::log::log_audio_warning(
            "[scrub] GRAIN dropped (overflow) at frame=%lld frames=%d",
            static_cast<long long>(seq_frame), f);
    if (dec_ms > 1.0)
        ::canvas::core::log::log_warning("[scrub] GRAIN frame=%lld decode_ms=%.2f samples=%zu",
                                     static_cast<long long>(seq_frame), dec_ms,
                                     s->samples.size());
}

const canvas::core::Clip* AudioPipeline::audio_clip_at(int64_t seq_frame) const {
    const auto sources = audible_sources_at(seq_frame);
    return sources.empty() ? nullptr : sources[0].clip;
}

std::vector<AudioPipeline::AudioSource> AudioPipeline::audible_sources_at(int64_t seq_frame) const {
    std::vector<AudioSource> out;
    if (project_ && seq_frame >= 0 && seq_frame < project_->sequence.duration_frames()) {
        const canvas::core::Sequence& seq = project_->sequence;

        bool any_solo = false;
        for (const auto& t : seq.audio_tracks)
            if (t.solo) { any_solo = true; break; }

        // Any audio-track clip under the playhead (enabled OR disabled/muted)
        // owns this frame's audio: never fall through to the video tracks, which
        // would let a disabled/muted audio clip be overridden by the movie's
        // embedded audio (breaking Ctrl+D mute and track mute).
        bool audio_track_covers = false;
        for (std::size_t i = seq.audio_tracks.size(); i-- > 0;) {
            const auto& track = seq.audio_tracks[i];
            const canvas::core::Clip* clip = track.clip_at(seq_frame);
            if (clip) audio_track_covers = true;
            if (!clip || !clip->enabled || clip->media < 0) continue;
            if (track.muted) continue;
            if (any_solo && !track.solo) continue;
            const double fps_v = media_fps_of(*project_, *clip);
            if (fps_v <= 0.0) continue;
            out.push_back({clip, fps_v, track.gain_db});
        }
        if (!audio_track_covers && !any_solo) {
            // No audio-track clip here (and nobody soloed): the video tracks'
            // embedded audio is the program audio, played like any other source.
            for (std::size_t i = seq.video_tracks.size(); i-- > 0;) {
                const auto& track = seq.video_tracks[i];
                const canvas::core::Clip* clip = track.clip_at(seq_frame);
                if (!clip || !clip->enabled || clip->media < 0) continue;
                const double fps_v = media_fps_of(*project_, *clip);
                if (fps_v <= 0.0) continue;
                out.push_back({clip, fps_v, 0.0f});
            }
        }
    }
    return out;
}

// The heart of the multi-track playback mix. Every audible source at `seq_frame`
// contributes a `want_frames`-long chunk (per-media feed watermark honoring, so
// pre-rolled lead-ins are never re-written): decode -> fade envelope -> clip
// Volume/Pan (balance) -> track gain -> N-channel mix (channels_) -> sum. The
// summed result is written to the device as one interleaved block. A
// single-source playhead at unity produces byte-for-byte the same samples the
// old single-clip path wrote.
int64_t AudioPipeline::write_mixed(int64_t seq_frame, int64_t want_frames) {
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty() || want_frames <= 0) return 0;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return 0;

    // Device mix bus in the pipeline's channel config (default stereo).
    std::vector<float> mix(static_cast<std::size_t>(want_frames) * channels_, 0.0f);
    int64_t out_frames = 0;

    const auto mix_t0 = Clock::now();
    for (const auto& src : sources) {
        const canvas::core::Clip& clip = *src.clip;
        // SEQUENCE-time mapping: every source advances one timeline-frame of
        // media per seq frame (src_in positions the trimmed start in the
        // source's own media fps). Sources with different frame rates or
        // samplerates therefore run in lockstep with the realtime clock — a
        // 60fps movie on a 30fps timeline plays its audio at 1:1, and music
        // alongside it is never sped up or slowed. Media-clock stepping would
        // feed `rate_/src.fps` samples per seq frame, halving or doubling the
        // stream for any source whose fps differs from the sequence.
        const int64_t start_sample = static_cast<int64_t>(std::llround(
            (static_cast<double>(clip.src_in) / src.fps +
             static_cast<double>(seq_frame - clip.tl_in) / seq_fps) *
            rate_));
        // Per-CLIP feed watermark (a media placed twice on the timeline has two
        // independent feed positions; keying by media let a clip change keep the
        // previous placement's head, making the new clip's audio start N seconds
        // in).
        const auto wit = feed_watermarks_.find(clip.id);
        const bool have_wm = wit != feed_watermarks_.end();
        const int64_t wm = have_wm ? wit->second : start_sample;
        const int64_t from = have_wm ? std::max(start_sample, wm) : start_sample;
        const int64_t span = start_sample + want_frames - from;
        if (cut_diag_ > 0 && span <= 0)
            ::canvas::core::log::log_warning(
                "[diag:cut]   STALE clip=%lld media=%d start=%lld wm=%lld span=%lld (write skip)",
                static_cast<long long>(clip.id), clip.media, static_cast<long long>(start_sample),
                static_cast<long long>(have_wm ? wit->second : start_sample),
                static_cast<long long>(span));
        if (span <= 0) continue;

        auto ait = decoders_.find(clip.media);
        if (ait == decoders_.end() || !ait->second->has_audio()) continue;
        // The AudioDecoder returns however many whole frames its fixed-length
        // chunk actually yields (a source that starts mid-span, or at EOF,
        // produces a shorter chunk than `want`). Request only the exact span
        // still owed from `from` so the chunk length reflects reality, and re-
        // query the remainder on the next step rather than assuming a full
        // `want` (the old fixed `want_frames` request let a short chunk appear
        // as a whole producing a silent zero-filled step).
        const auto dec_t0 = Clock::now();
        auto chunk = ait->second->decode(from, static_cast<int>(std::min<int64_t>(span, want_frames)),
                                         rate_);
        const double dec_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - dec_t0).count();
        step_decode_ms_ += dec_ms;
        if (!chunk || chunk->samples.empty()) continue;
        if (playback_dbg() || cut_diag_ > 0) {
            const int dbg_ch = chunk->channels > 0 ? chunk->channels : 1;
            const int dbg_frames = static_cast<int>(chunk->samples.size()) / dbg_ch;
            ::canvas::core::log::log_warning(
                "[audio] DECODE media=%d from=%lld span=%lld dec_ms=%.2f frames=%d",
                clip.media, static_cast<long long>(from),
                static_cast<long long>(span), dec_ms, dbg_frames);
        }
        // Never advance a source's watermark past its own clip end: a decode
        // that ran out (EOF / fixed-length chunk) must be re-requested from the
        // same `from` on the next step instead of silently skipping its tail.
        const int src_ch = chunk->channels > 0 ? chunk->channels : 1;
        const int frames = static_cast<int>(chunk->samples.size()) / src_ch;
        if (frames <= 0) continue;
        const int64_t src_out_sample = static_cast<int64_t>(
            std::llround(static_cast<double>(clip.src_out) / src.fps * rate_));
        const int64_t chunk_end = from + static_cast<int64_t>(frames);
        feed_watermarks_[clip.id] = std::max(
            wit != feed_watermarks_.end() ? wit->second : start_sample,
            std::min<int64_t>(chunk_end, src_out_sample));
        if (cut_diag_ > 0)
            ::canvas::core::log::log_warning(
                "[diag:cut]   SRC clip=%lld media=%d start=%lld from=%lld span=%lld frames=%d "
                "wm=%lld src_out_sample=%lld",
                static_cast<long long>(clip.id), clip.media, static_cast<long long>(start_sample),
                static_cast<long long>(from), static_cast<long long>(span), frames,
                static_cast<long long>(feed_watermarks_[clip.id]),
                static_cast<long long>(src_out_sample));

<<<<<<< Updated upstream
        mix_source_chunk(mix, channels_, clip, src.gain_db, chunk, from, rate_, src.fps, seq_fps);
        out_frames = std::max(out_frames, static_cast<int64_t>(frames));
=======
        // Per-clip Speed Change + Pitch shift + Pan (WSOLA + windowed-sinc
        // retime engine): stretch the decoded media to output frames sized by
        // the realtime clock (want_frames), with the pitch-shifted resample
        // and the pan balance baked into the engine output. The engine
        // streams per clip and returns fewer frames than requested
        // only while it primes its first ~40 ms window; those gaps even out
        // over the next feed (RNNoise behaves the same), and the mix path
        // writes exactly what it gets.
        std::vector<float> sped;
        const float* pcm = chunk->samples.data();
        int den_ch = src_ch;
        int den_frames = frames;
        const double pitch = canvas::core::cliprate::pitch_factor(clip);
        const bool need_dsp = spd != 1.0 || pitch != 1.0 || clip.pan != 0.0f;
        if (need_dsp) {
            const int want = static_cast<int>(std::max<int64_t>(
                1, std::min<int64_t>(
                       canvas::core::cliprate::output_frames_from_media(
                           clip, static_cast<int64_t>(frames)),
                       want_frames)));
            int out_ch = den_ch;
            const int written = stretch_bank_.tick(clip.id, spd, pitch, clip.pan, rate_,
                                                   src_ch, chunk->samples.data(), frames,
                                                   want, sped, &out_ch);
            if (written <= 0) continue;  // stretch priming: nothing to mix this step
            pcm = sped.data();
            den_frames = written;
            den_ch = out_ch;
        }

        // Per-clip AI voice isolation (RNNoise): denoise this source's PCM
        // BEFORE it mixes, at the pipeline's native 48 kHz. The bank streams
        // per clip and returns fewer frames only while the network primes
        // (<=10 ms); those frames are mixed exactly as decoded would be.
        std::vector<float> denoised;
        if (clip.voice_isolation != canvas::core::VoiceIsolationMode::None) {
            denoised.assign(pcm, pcm + static_cast<std::size_t>(den_frames) * den_ch);
            const int written = iso_bank_.tick(clip.id, clip.voice_isolation, rate_, den_ch,
                                               denoised.data(), den_frames);
            if (written > 0) {
                pcm = denoised.data();
                den_frames = written;
            } else {
                pcm = nullptr;
                den_frames = 0;
            }
        }
        if (pcm && den_frames > 0) {
            // Per-clip parametric EQ: the 6-band biquad cascade (RBJ Cookbook)
            // shapes the denoised PCM BEFORE its gains/mix — the same stage
            // order as export. The EQ is pure stream-in-place filtering at any
            // rate (no fixed-48 kHz rule like RNNoise) and always writes the
            // same frame count it was given, so the mix accounting is
            // untouched. Disabled clips pass through bit-exact; the bank KEEPS
            // the per-clip filter state across a toggle (disable = pass-through
            // with carried DF2T state), so re-enabling EQ resumes sample-
            // continuously instead of cold-starting a zero-state filter whose
            // first output sample would jump to ~b0*x (a click/static per
            // toggle). Only drop() (discontinuous seek/rewind) clears it.
            std::vector<float> eqd;
            if (clip.eq_enabled || eq_bank_.wants_samples(clip.id, false)) {
                // Feed real PCM whenever EQ is on OR the bank is still gliding
                // dry->wet / wet->dry on a toggle edge (the crossfade needs the
                // raw input to blend against). Once a disabled clip settles dry
                // the bank reports wants_samples()==false and we hand it the
                // null/0 no-op call, keeping the disabled-fast path copy-free.
                eqd.assign(pcm, pcm + static_cast<std::size_t>(den_frames) * den_ch);
                // The EQ is stream-in-place and rate-preserving, so it writes
                // exactly the frames it was given (the returned count is the
                // no-lookahead contract, asserted here).
                den_frames = eq_bank_.tick(clip.id, clip.eq_bands, clip.eq_enabled, rate_,
                                           den_ch, eqd.data(), den_frames);
                pcm = eqd.data();
            } else {
                (void)eq_bank_.tick(clip.id, clip.eq_bands, false, rate_, den_ch, nullptr, 0);
            }
        }
        if (pcm && den_frames > 0) {
            // Pan is baked by the retime engine for the DSP path, so the mix
            // applies center (the boundary grain/feed paths pass clip.pan).
            mix_source_samples(mix, channels_, clip, src.gain_db,
                               effective_clip_volume_db(clip), pcm, den_ch, den_frames, from,
                               rate_, src.fps, seq_fps, spd, 0.0f);
        }
        out_frames = std::max(out_frames, static_cast<int64_t>(den_frames));
>>>>>>> Stashed changes
    }
    step_mix_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - mix_t0).count();

    if (out_frames <= 0) return 0;
    const auto write_t0 = Clock::now();
    sink_.write_float(mix.data(), static_cast<int>(out_frames));
    step_write_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - write_t0).count();
    // Diagnostic: capture exactly what the device received, and flag mix sums
    // above 0 dBFS (music+movie summed hot has no headroom here; the float bus
    // passes >1.0 straight to the DAC, which clips -> the "nasty/garbled" report).
    maybe_wave_capture_open_locked();
    wave_capture_write_locked(mix.data(), static_cast<std::size_t>(out_frames));
    float mix_peak = 0.0f;
    bool mix_nonfinite = false;
    const std::size_t mix_len = static_cast<std::size_t>(out_frames) * channels_;
    for (std::size_t k = 0; k < mix_len; ++k) {
        if (std::isnan(mix[k]) || std::isinf(mix[k])) {
            mix_nonfinite = true;
            break;
        }
        mix_peak = std::max(mix_peak, std::fabs(mix[k]));
    }
    static auto last_clip_log = Clock::now();
    if (mix_nonfinite && Clock::now() - last_clip_log >= std::chrono::seconds(1)) {
        last_clip_log = Clock::now();
        ::canvas::core::log::log_audio_warning(
            "audio: MIX NONFINITE -> DAC garbage: sources=%zu frame=%lld",
            sources.size(), static_cast<long long>(seq_frame));
    } else if (mix_peak > 1.0f && Clock::now() - last_clip_log >= std::chrono::seconds(1)) {
        last_clip_log = Clock::now();
        ::canvas::core::log::log_audio_warning(
            "audio: MIX exceeds 0 dBFS -> DAC clips: peak=%.3f sources=%zu frame=%lld",
            static_cast<double>(mix_peak), sources.size(), static_cast<long long>(seq_frame));
    }
    return out_frames;
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
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return 0;
    // Mirror write_mixed(): src_in positions the clip in the source's own media
    // time; the seq-frame offset advances by SEQUENCE time (1:1 realtime).
    return static_cast<int64_t>(std::llround(
        (static_cast<double>(clip->src_in) / fps_v +
         static_cast<double>(seq_frame - clip->tl_in) / seq_fps) *
        rate_));
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
            const double seq_fps = project_->sequence.fps;
            if (seq_fps <= 0.0) continue;
            // Inverse of playhead_to_audio_sample(): the sample is positioned in
            // the source's media time (src_in/src_out in that fps), so translate
            // back to seconds-into-the-trim then to SEQ frames at the sequence
            // rate. Only interpreted within [src_in, src_out).
            const double secs_in =
                static_cast<double>(media_sample) / rate_ - static_cast<double>(c.src_in) / fps_v;
            if (secs_in < 0.0 ||
                secs_in >= static_cast<double>(c.src_out - c.src_in) / fps_v)
                continue;  // outside this clip's trim
            const int64_t seq_frame =
                c.tl_in + static_cast<int64_t>(std::llround(secs_in * seq_fps));
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
    // Audible position within the current audio run. Seq-domain: the anchor's
    // seq frame + the media the device consumed since (1:1 to sequence time).
    const uint64_t audible_frames = sink_.audible_position_frames();
    const int64_t run_audible =
        static_cast<int64_t>(audible_frames) - static_cast<int64_t>(written_at_anchor_);
    const double audible_ms =
        static_cast<double>(anchor_seq_frame_) / video_fps * 1000.0 +
        static_cast<double>(run_audible) / static_cast<double>(rate_) * 1000.0;

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

<<<<<<< Updated upstream
    ::canvas::core::log::log_warning(
=======
    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
        "[avsync] run=%llu frame=%lld video_ms=%.1f audible_ms=%.1f av_offset_ms=%.1f "
        "speed_x=%.3f write_to_audible_ms=%.1f step_s=%.4f",
        static_cast<unsigned long long>(run_id_), static_cast<long long>(seq_frame), video_ms,
        audible_ms, video_ms - audible_ms, speed_ratio, written_ms - audible_ms, step_seconds);
}

void AudioPipeline::play_step(int64_t seq_frame, double step_seconds, bool seek_hold_active) {
    std::lock_guard lock(mutex_);
    // Fresh per-call budget (write_mixed accumulates into them).
    step_decode_ms_ = 0.0;
    step_mix_ms_ = 0.0;
    step_write_ms_ = 0.0;
    // Throttle failure warnings to ~1/s since this runs every frame.
    static auto last_warn = Clock::now();
    const auto warn = [&](const char* why) {
        const auto now = Clock::now();
        if (now - last_warn < std::chrono::seconds(1)) return;
        last_warn = now;
        ::canvas::core::log::log_audio_warning("audio: no output at frame %lld -> %s",
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
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty()) {
        // Ghost-audio diagnostic: tell a clip that's present but DISABLED/muted/
        // soloed-out apart from no clip at all. Hitting this while the user still
        // hears audio means the pipeline's project is stale (the mute never landed).
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
<<<<<<< Updated upstream
                    ::canvas::core::log::log_warning(
=======
                    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
                        "audio: GHOST-GUARD frame=%lld enabled=%d media=%d tl=%lld->%lld suppressed=%d",
                        static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                        static_cast<long long>(present->tl_in),
                        static_cast<long long>(present->tl_out), gg_suppressed_);
                else
<<<<<<< Updated upstream
                    ::canvas::core::log::log_warning(
=======
                    ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
                        "audio: GHOST-GUARD frame=%lld enabled=%d media=%d tl=%lld->%lld",
                        static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                        static_cast<long long>(present->tl_in),
                        static_cast<long long>(present->tl_out));
                gg_suppressed_ = 0;
            } else {
                ++gg_suppressed_;
            }
        }
        warn(present && present->media >= 0 ? "ghost-guard: clip present but (muted/soloed/disabled)"
                                            : "no audio clip at playhead");
        return;
    }

    const canvas::core::Clip* clip = sources[0].clip;
    const double seq_fps = project_->sequence.fps;
    const double fps_v = sources[0].fps;
    if (fps_v <= 0.0) {
        warn("media fps unknown for audio clip");
        return;
    }
    if (seq_fps <= 0.0) {
        warn("sequence fps unknown for audio clip");
        return;
    }

    const int64_t start_sample = static_cast<int64_t>(std::llround(
        (static_cast<double>(clip->src_in) / fps_v +
         static_cast<double>(seq_frame - clip->tl_in) / seq_fps) *
        rate_));
    const int64_t want = std::max<int64_t>(1, static_cast<int64_t>(std::llround(step_seconds * rate_)));
    // Always-ON A/V sync (throttled to ~1/s internally). Uses the device's true
    // audible position anchored to this run, so default captures show the real
    // audio-vs-video offset and let us compare run N (before scrub) vs N+1.
    log_av_sync(seq_frame, seq_fps, step_seconds);

    // The master's feed watermark start (for diagnostics + the skip check). The
    // per-source watermarks live inside write_mixed; this mirrors its `from`.
    const auto wit = feed_watermarks_.find(clip->id);
    const int64_t from =
        wit != feed_watermarks_.end() ? std::max(start_sample, wit->second) : start_sample;

    // === A/V DRIFT RE-ANCHOR (chronic offset) ===
    // A long video stall (decode-capped media under the playhead) lets the
    // device keep consuming at realtime while the picture is frozen, so the
    // audible position runs many seconds ahead of the frame shown; the reverse
    // (audible behind) follows a long write-stall. Neither heals by itself when
    // both clocks later advance 1:1 — the field log's permanent ±9.6s offset.
    // Re-anchor whenever the drift exceeds ~2s so audio snaps back to the
    // picture instead of playing the future/past forever. Throttled to ~1/s:
    // during a prolonged stall this re-anchors repeatedly, keeping audio glued
    // to the frozen frame rather than running a quarter minute ahead.
    if (last_drift_anchor_.time_since_epoch().count() == 0 ||
        Clock::now() - last_drift_anchor_ >= std::chrono::seconds(1)) {
        const int64_t run_audible =
            static_cast<int64_t>(sink_.audible_position_frames()) -
            static_cast<int64_t>(written_at_anchor_);
        // Seq-domain audible: at the re-anchor the audible content equals the
        // picture at `anchor_seq_frame_`; the device has since consumed `run_audible`
        // media samples, which the 1:1 feed maps to elapsed sequence time. (The raw
        // media sample would be in the SOURCE's domain — off by the clip's src_in
        // placement — causing phantom drift on trimmed clips.)
        const double audible_ms =
            static_cast<double>(anchor_seq_frame_) / seq_fps * 1000.0 +
            static_cast<double>(run_audible) / static_cast<double>(rate_) * 1000.0;
        const double video_ms = static_cast<double>(seq_frame) / seq_fps * 1000.0;
        const int64_t drift_ms = std::llround(audible_ms - video_ms);
        constexpr int64_t kMaxAvDriftMs = 2000;
        if (std::llabs(drift_ms) > kMaxAvDriftMs) {
            last_drift_anchor_ = Clock::now();
::canvas::core::log::log_audio_warning(
            "audio: A/V DRIFT re-anchor offset_ms=%lld audible_ms=%.1f video_ms=%.1f",
            static_cast<long long>(drift_ms), audible_ms, video_ms);
            reanchor_locked(seq_frame);
        }
    }

    // === SELF-HEALING RE-ANCHOR on a playhead discontinuity ===
    // Every legit reposition path (scrub release, transport slider release,
    // drop-to-realtime) re-anchors audio through rewind(). But some playhead
    // moves arrive WITHOUT a committed seek — a transport-wheel scrub, a
    // timeline nudge, a preview-only drag release. When that happens the old
    // run's front watermark stays ahead of the new playhead, `from` lands past
    // `start_sample + want`, write_mixed skips every source (`span <= 0`), the
    // step writes nothing, and the device keeps playing the previous run's
    // buffered audio ahead of the picture — the garbled/desynced-audio
    // regression in the field log (permanent `wrote=0` + audible 9s ahead).
    // Detect the jump here and re-anchor in place so playback survives it.
    // Backward move of more than one frame is always a reposition.
    const bool backward_jump = last_seq_fed_ >= 0 && seq_frame < last_seq_fed_ - 1;
    // Forward move whose lap is so far ahead of the feed that the audio would
    // trail by ~1s of media (a fast wheel/slider hop with no commit): re-anchor
    // so the audible content follows the picture instead of replaying the gap.
    constexpr int64_t kForwardReanchorSamples = 48000;  // ~1s at 48kHz
    const bool forward_hop =
        last_seq_fed_ >= 0 && start_sample - from > kForwardReanchorSamples;
    if (backward_jump || forward_hop) {
        ::canvas::core::log::log_audio_warning(
            "audio: playhead discontinuity at frame=%lld (last_fed=%lld %s) — self re-anchor",
            static_cast<long long>(seq_frame), static_cast<long long>(last_seq_fed_),
            backward_jump ? "backward" : "forward-hop");
        reanchor_locked(seq_frame);
        // Watermarks cleared by the re-anchor: the mix below starts at the new
        // playhead instead of the stale old-run front.
    }

    // MIX every audible source at the playhead (per-source volume/pan + fade
    // envelopes + mute/solo) and hand the summed stereo block to the device.
    //
    // DEVICE-PACED FEED: the sink's audible clock is the master (the picture
    // cadence only offers a frame every 33ms; the SOUND moves at the device's
    // pace). Previously the mix wrote `want` every step *plus* an independent
    // pending_frames-keyed top-up, so whenever the `default` (PulseAudio/
    // PipeWire) adapter drained slower than realtime the system backlog
    // (device delay + queue == stat_written - audible) ballooned ~1s and the
    // picture ran that far ahead of the sound, taking seconds to drain — the
    // ±1s A/V wobble in the 23:18 capture, peaking around the 12s and 21.4s
    // timeline cuts. Now the feed is self-limiting on the device: defer the
    // step while the sink already holds more than the kAudioLeadMs horizon
    // plus one step, so a slow-draining sink sheds its backlog instead of
    // being staffed, and refill (via a watermark-poking retry) only when the
    // device is actually short of the lead.
    const int64_t backlog_f = static_cast<int64_t>(sink_.stat_written_frames()) -
                              static_cast<int64_t>(sink_.audible_position_frames());
    const int64_t lead_f = static_cast<int64_t>(
        static_cast<double>(rate_) * kAudioLeadMs / 1000.0);
    const bool deferred = backlog_f > lead_f + want;
    int64_t written = deferred ? 0 : write_mixed(seq_frame, want);
    last_seq_fed_ = std::max(last_seq_fed_, seq_frame);
    if (!deferred && written <= 0 && backlog_f < lead_f) {
        // Start-of-run / preroll head: the front watermark still sits ahead of
        // the playhead (`span <= 0`), so the step above wrote nothing while the
        // device is short of the lead — poke once more with an extra slice past
        // the stale watermark so the refill isn't starved by its own bookkeeping.
        const int64_t deficit = lead_f - backlog_f;
        const int64_t extra = std::min<int64_t>(deficit, want);
        written = write_mixed(seq_frame, want + static_cast<int64_t>(extra));
    }
    // Cut-window diagnostic: while the playhead is near a clip boundary (a clip
    // change, or ±3 frames of tl_in/tl_out) log every frame so the cut-time
    // ledger — the "no output" stall, the cold-decoder short chunk, the
    // device-backlog balloon — is visible frame-by-frame instead of collapsed
    // into the ~1Hz [audio:feed] line. Unconditional like the other [audio]
    // lines (the ~40-frame window at each cut costs ~150 lines per cut), so it
    // lands in the default ~/studio/canvas_debug.log without a debug flag.
    {
        const bool clip_changed = clip->id != last_primary_clip_;
        last_primary_clip_ = clip->id;
        const bool near_start = seq_frame < clip->tl_in + 4;
        const bool near_end = clip->tl_out > 0 && seq_frame > clip->tl_out - 4;
        if (clip_changed || near_start || near_end) cut_diag_ = 40;
        if (clip_changed)
<<<<<<< Updated upstream
            ::canvas::core::log::log_warning(
=======
            ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
                "audio: CUT-BOUNDARY clip=%lld media=%d tl=%lld->%lld src=%lld->%lld frame=%lld",
                static_cast<long long>(clip->id), clip->media, static_cast<long long>(clip->tl_in),
                static_cast<long long>(clip->tl_out), static_cast<long long>(clip->src_in),
                static_cast<long long>(clip->src_out), static_cast<long long>(seq_frame));
        if (cut_diag_ > 0) {
            --cut_diag_;
            const uint64_t acc = sink_.audible_position_frames();
            const int64_t run_aud =
                static_cast<int64_t>(acc) - static_cast<int64_t>(written_at_anchor_);
            const double a_ms =
                static_cast<double>(anchor_seq_frame_) / seq_fps * 1000.0 +
                static_cast<double>(run_aud) / static_cast<double>(rate_) * 1000.0;
            const double v_ms = static_cast<double>(seq_frame) / seq_fps * 1000.0;
            const double lat_ms =
                static_cast<double>(static_cast<int64_t>(sink_.stat_written_frames()) -
                                    static_cast<int64_t>(acc)) /
                static_cast<double>(rate_) * 1000.0;
            const double pend_ms =
                static_cast<double>(sink_.pending_frames()) / static_cast<double>(rate_) * 1000.0;
            ::canvas::core::log::log_warning(
                "[diag:cut] frame=%lld clip=%lld media=%d wrote=%lld want=%lld from=%lld "
                "audible_ms=%.1f video_ms=%.1f av_offset_ms=%.1f lat_ms=%.1f pend_ms=%.1f "
                "backlog_ms=%.1f dec_ms=%.2f mix_ms=%.2f write_ms=%.2f",
                static_cast<long long>(seq_frame), static_cast<long long>(clip->id), clip->media,
                static_cast<long long>(written), static_cast<long long>(want),
                static_cast<long long>(from), a_ms, v_ms, a_ms - v_ms, lat_ms, pend_ms,
                static_cast<double>(backlog_f) / static_cast<double>(rate_) * 1000.0,
                step_decode_ms_, step_mix_ms_, step_write_ms_);
        }
    }
    if (written <= 0 && !deferred) {
        if (playback_dbg())
            ::canvas::core::log::log_audio_warning(
                "audio: play step SKIPPED seq_frame=%lld start_sample=%lld from=%lld want=%lld",
                static_cast<long long>(seq_frame), static_cast<long long>(start_sample),
                static_cast<long long>(from), static_cast<long long>(want));
        warn("decode produced no samples");
    }

    // Always-on audio feed health (~1/s): media position just handed to the device,
    // how many frames, and the offset vs the frame being presented. Churn/gaps
    // are what the listener hears as artifacts.
    static auto last_afe_log = Clock::now();
    static int afe_ = 0;
    static uint64_t last_resync_total = 0;
    if ((++afe_) == 1 || Clock::now() - last_afe_log >= std::chrono::seconds(1)) {
        last_afe_log = Clock::now();
        static int64_t last_start = 0;
        // Container-resync delta across every open decoder since the last line:
        // steady playback must never resync, so any growth here means the feed
        // keeps jumping outside the decoded window (watermark/playhead mismatch).
        uint64_t resync_total = 0;
        for (const auto& [id, adec] : decoders_) resync_total += adec->resync_count();
        const uint64_t resync_delta = resync_total - last_resync_total;
        // Samples handled to the device on this call (not the seek-jump media delta):
        // a value far above the per-frame `want` means a burst.
        const int64_t written_now = written;
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
            static_cast<double>(anchor_seq_frame_) / seq_fps * 1000.0 +
            static_cast<double>(run_audible) / static_cast<double>(rate_) * 1000.0;
        const double video_ms = static_cast<double>(seq_frame) / seq_fps * 1000.0;
<<<<<<< Updated upstream
        ::canvas::core::log::log_warning(
=======
        ::canvas::core::log::log_audio_info(
>>>>>>> Stashed changes
            "[audio] frame=%lld wrote=%lld req=%lld seek_hop=%d burst=%d audible_ms=%.1f "
            "video_ms=%.1f av_offset_ms=%.1f churn=%d dec_ms=%.2f mix_ms=%.2f write_ms=%.2f "
            "resyncs=%llu",
            static_cast<long long>(seq_frame), static_cast<long long>(written_now),
            static_cast<long long>(want), (int)seek_hop, (int)burst, audible_ms, video_ms,
            audible_ms - video_ms, (int)(written_now == 0), step_decode_ms_, step_mix_ms_,
            step_write_ms_,
            static_cast<unsigned long long>(resync_delta));
        last_resync_total = resync_total;
        log_feed_ledger_locked(seq_frame, start_sample, from, want, written);
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
    const auto sources = audible_sources_at(target);
    if (sources.empty()) return;
    const canvas::core::Clip* clip = sources[0].clip;
    if (!clip || clip->media < 0) return;
    auto ait = decoders_.find(clip->media);
    if (ait == decoders_.end() || !ait->second->has_audio()) return;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return;
    // Sequence-time mapping (mirrors write_mixed): the landed scrub position
    // voices the source's audio at its own native speed on the realtime clock.
    const int64_t base_sample = static_cast<int64_t>(std::llround(
        (static_cast<double>(clip->src_in) / fps_v +
         static_cast<double>(target - clip->tl_in) / seq_fps) *
        rate_));
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
        std::vector<float> chunk_mix(static_cast<std::size_t>(frames) * channels_, 0.0f);
        mix_source_chunk(chunk_mix, channels_, *clip, sources[0].gain_db, s, base_sample,
                         rate_, fps_v, seq_fps);
        ok = sink_.reposition_enqueue(chunk_mix.data(), frames);
    }
    const std::size_t pending_after = sink_.pending_frames();
    if (ok) ++scrub_repositions_;
    last_scrub_audio_target_ = target;
    const double target_ms = static_cast<double>(target) / seq_fps * 1000.0;
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
    if (!active_) return;
    const canvas::core::Clip* ac = audio_clip_at(new_frame);
    if (!ac || ac->media < 0) return;
    const double afps = media_fps_of(*project_, *ac);
    if (afps > 0.0) {
        const double seq_fps = project_->sequence.fps;
        if (seq_fps > 0.0) {
            // Sequence-time mapping like write_mixed(), so the forward-advanced
            // watermark matches what the next mix computes as its own start.
            const int64_t asrc = static_cast<int64_t>(std::llround(
                (static_cast<double>(ac->src_in) / afps +
                 static_cast<double>(new_frame - ac->tl_in) / seq_fps) *
                rate_));
            int64_t& wm = feed_watermarks_[ac->id];
            wm = std::max(wm, asrc);
        }
    }
}

canvas::core::MediaId AudioPipeline::audio_media_at(int64_t seq_frame) const {
    std::lock_guard lock(mutex_);
    const canvas::core::Clip* c = audio_clip_at(seq_frame);
    return c ? c->media : -1;
}

}  // namespace canvas::gui