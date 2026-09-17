#include "audio_pipeline.hpp"

#include "audio_sink.hpp"
#include "sync_constants.hpp"
#include "canvas/core/timeline/audio_fade.hpp"
#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/timeline/clip_rate.hpp"
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
bool playback_dbg() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_PLAYBACK_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

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

void compute_fade_gains(const canvas::core::Clip& clip, const int64_t from_sample,
                        const int out_rate, const double src_fps, const double seq_fps,
                        const double speed, const int frames, std::vector<float>* out) {
    const bool has_fade =
        (clip.transition_in_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_in)) ||
        (clip.transition_out_duration > 0 &&
         canvas::core::is_audio_transition(clip.transition_out));
    if (!has_fade) return;
    const double base_sample = static_cast<double>(clip.src_in) / src_fps * out_rate;
    out->resize(static_cast<std::size_t>(frames));
    for (int k = 0; k < frames; ++k) {
        const double media_off = (static_cast<double>(from_sample) - base_sample) +
                                 static_cast<double>(k) * speed;
        const int64_t tl = clip.tl_in + static_cast<int64_t>(std::floor(
            media_off / out_rate * seq_fps / speed));
        (*out)[static_cast<std::size_t>(k)] = canvas::core::audio_fade_gain(clip, tl);
    }
}

void mix_source_samples(std::vector<float>& out, const int out_channels,
                        const canvas::core::Clip& clip, const float track_gain_db,
                        const float volume_db, const float* samples, const int src_ch,
                        const int frames, const int64_t from_sample, const int out_rate,
                        const double src_fps, const double seq_fps, const double speed = 1.0,
                        const float pan = 0.0f) {
    if (frames <= 0 || !samples) return;
    std::vector<float> gains;
    compute_fade_gains(clip, from_sample, out_rate, src_fps, seq_fps, speed, frames, &gains);
    float gl = 1.0f, gr = 1.0f;
    canvas::core::audio_mix::pan_gains(pan, gl, gr);
    const float vol = canvas::core::audio_mix::db_to_gain(volume_db) *
                      canvas::core::audio_mix::db_to_gain(track_gain_db);
    canvas::core::audio_mix::mix_chunk(out, out_channels, samples, src_ch, frames,
                                       gains.empty() ? nullptr : &gains, vol, gl, gr);
}

void mix_source_chunk(std::vector<float>& out, const int out_channels,
                      const canvas::core::Clip& clip, const float track_gain_db,
                      const float volume_db, const canvas::core::AudioChunkPtr& chunk,
                      const int64_t from_sample, const int out_rate, const double src_fps,
                      const double seq_fps, const double speed = 1.0,
                      const float pan = 0.0f) {
    const int src_ch = chunk->channels > 0 ? chunk->channels : 1;
    const int frames = static_cast<int>(chunk->samples.size()) / src_ch;
    mix_source_samples(out, out_channels, clip, track_gain_db, volume_db,
                       chunk->samples.data(), src_ch, frames, from_sample, out_rate,
                       src_fps, seq_fps, speed, pan);
}
}

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
        CANVAS_LOG("audio: opened decoder for media %d rate=%d ch=%d",
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
    last_primary_clip_ = 0;
    cut_diag_ = 0;
    live_gain_db_.clear();
    iso_bank_.clear();
    stretch_bank_.clear();
    eq_bank_.clear();
}

void AudioPipeline::set_live_clip_gain(canvas::core::ClipId id, float volume_db) {
    std::lock_guard lock(mutex_);
    live_gain_db_[id] = volume_db;
}

void AudioPipeline::clear_live_clip_gain(canvas::core::ClipId id) {
    std::lock_guard lock(mutex_);
    live_gain_db_.erase(id);
}

void AudioPipeline::clear_live_clip_gains() {
    std::lock_guard lock(mutex_);
    live_gain_db_.clear();
}

float AudioPipeline::effective_clip_volume_db(const canvas::core::Clip& clip) const {
    const auto it = live_gain_db_.find(clip.id);
    return it != live_gain_db_.end() ? it->second : clip.volume_db;
}

void AudioPipeline::open_output() {
    std::lock_guard lock(mutex_);
    if (active_) return;
    active_ = sink_.open(rate_, channels_);
    if (!active_)
        ::canvas::core::log::log_audio_warning(
            "audio: FAILED to open output device rate=%d channels=%d", rate_, channels_);
    CANVAS_LOG("audio: pipeline open output active=%d rate=%d channels=%d", (int)active_, rate_,
               channels_);
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
    CANVAS_LOG("audio: pipeline rewind active=%d playing=%d", (int)active_, (int)playing);
    reanchor_locked(seq_frame);
}

void AudioPipeline::reanchor_locked(int64_t seq_frame) {
    ++run_id_;
    ::canvas::core::log::log_audio_info(
        "[avsync] RE-ANCHOR run=%llu at_seq_frame=%lld device_written=%llu",
        static_cast<unsigned long long>(run_id_), static_cast<long long>(seq_frame),
        static_cast<unsigned long long>(sink_.stat_written_frames()));
    written_at_anchor_ = sink_.stat_written_frames();
    anchor_media_sample_ = playhead_to_audio_sample(seq_frame);
    anchor_seq_frame_ = seq_frame;
    feed_watermarks_.clear();
    last_seq_fed_ = seq_frame;
    CANVAS_LOG("audio: rewind reset feed, anchor_media_sample=%lld rate=%d",
               static_cast<long long>(anchor_media_sample_), rate_);
    for (auto& [id, adec] : decoders_) adec->reset();
    iso_bank_.drop();
    stretch_bank_.drop();
    eq_bank_.drop();
    if (active_) {
        sink_.log_pipeline_stats("rewind-pre");
        sink_.flush();
        sink_.log_pipeline_stats("rewind-post");
    }
}

void AudioPipeline::preroll(int64_t seq_frame, int lead_ms, bool playing) {
    std::lock_guard lock(mutex_);
    if (!active_ || !sink_.is_open() || lead_ms <= 0) return;
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty()) return;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return;
    const int64_t total = static_cast<int64_t>(static_cast<double>(lead_ms) / 1000.0 * rate_);
    const int64_t written = write_mixed(seq_frame, total);
    CANVAS_LOG("audio: preroll lead_ms=%d written_frames=%lld sources=%zu", lead_ms,
               static_cast<long long>(written), sources.size());
    ::canvas::core::log::log_audio_info(
        "[audio:preroll] cur=%lld base_sample=%lld written_frames=%lld audible_before=%llu playing=%d",
        static_cast<long long>(seq_frame), static_cast<long long>(playhead_to_audio_sample(seq_frame)),
        static_cast<long long>(written),
        static_cast<unsigned long long>(sink_.audible_position_frames()), (int)playing);
}

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
    const uint32_t sr = static_cast<uint32_t>(rate_);
    const uint32_t byte_rate = sr * static_cast<uint32_t>(channels_) * 4u;
    const uint16_t block_align = static_cast<uint16_t>(channels_ * 4);
    std::fwrite("RIFF", 1, 4, wav_capture_f_);
    const uint32_t riff_tmp = 0;
    std::fwrite(&riff_tmp, 4, 1, wav_capture_f_);
    std::fwrite("WAVEfmt ", 1, 8, wav_capture_f_);
    const uint32_t fmt_sz = 16;
    std::fwrite(&fmt_sz, 4, 1, wav_capture_f_);
    const uint16_t fmt_id = 3;
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
    CANVAS_LOG("audio: wave capture ON -> %s rate=%d ch=%d", path, rate_, channels_);
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
    ::canvas::core::log::log_audio_info(
        "audio: wave capture OFF frames=%llu path=%s",
        static_cast<unsigned long long>(wav_capture_frames_), wav_capture_path_.c_str());
}

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
    ::canvas::core::log::log_audio_info(
        "[audio:feed] frame=%lld start=%lld from=%lld want=%lld wrote=%lld fed_window=%llu "
        "dev_written_delta=%lld (%.0f/s) pending=%zu (%.0fms) audible=%llu dev_lat_ms=%.1f",
        static_cast<long long>(seq_frame), static_cast<long long>(start_sample),
        static_cast<long long>(from), static_cast<long long>(want), static_cast<long long>(written),
        static_cast<unsigned long long>(feed_ledger_frames_), static_cast<long long>(dev_delta),
        static_cast<double>(dev_delta) / secs, sink_.pending_frames(),
        static_cast<double>(sink_.pending_frames()) / static_cast<double>(rate_) * 1000.0,
        static_cast<unsigned long long>(sink_.audible_position_frames()),
        static_cast<double>(static_cast<int64_t>(dev_written) -
                            static_cast<int64_t>(sink_.audible_position_frames())) /
            static_cast<double>(rate_) * 1000.0);
    feed_ledger_frames_ = 0;
    sink_.log_pipeline_stats("feed");
}

void AudioPipeline::play_scrub_grain(int64_t seq_frame) {
    std::lock_guard lock(mutex_);
    if (!project_ || !active_ || !sink_.is_open()) return;
    const auto sources = audible_sources_at(seq_frame);
    const canvas::core::Clip* clip = sources.empty() ? nullptr : sources[0].clip;
    if (!clip || clip->media < 0) {
        if (project_) {
            if (const auto* present = clip_at_any_track(seq_frame))
                CANVAS_LOG(
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
        static_cast<int64_t>(static_cast<double>(40) / 1000.0 * rate_);
    const auto g0 = Clock::now();
    auto s = ait->second->decode(base_sample, static_cast<int>(grain_frames), rate_);
    const double dec_ms = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
    if (!s || s->samples.empty()) return;
    const int f = static_cast<int>(s->samples.size() / s->channels);
    if (f <= 0) return;
    std::vector<float> grain(static_cast<std::size_t>(f) * channels_, 0.0f);
    mix_source_chunk(grain, channels_, *clip, sources[0].gain_db,
                     effective_clip_volume_db(*clip), s, base_sample, rate_, fps_v, seq_fps,
                     1.0, clip->pan);
    if (!sink_.write_float(grain.data(), f))
        ::canvas::core::log::log_audio_warning(
            "[scrub] GRAIN dropped (overflow) at frame=%lld frames=%d",
            static_cast<long long>(seq_frame), f);
    if (dec_ms > 1.0)
        CANVAS_LOG("[scrub] GRAIN frame=%lld decode_ms=%.2f samples=%zu",
                   static_cast<long long>(seq_frame), dec_ms, s->samples.size());
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

int64_t AudioPipeline::write_mixed(int64_t seq_frame, int64_t want_frames) {
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty() || want_frames <= 0) return 0;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return 0;

    std::vector<float> mix(static_cast<std::size_t>(want_frames) * channels_, 0.0f);
    int64_t out_frames = 0;

    const auto mix_t0 = Clock::now();
    for (const auto& src : sources) {
        const canvas::core::Clip& clip = *src.clip;
        const double spd = canvas::core::cliprate::effective_rate(clip);
        const int64_t start_sample = static_cast<int64_t>(std::llround(
            (static_cast<double>(clip.src_in) / src.fps +
             (static_cast<double>(seq_frame - clip.tl_in) / seq_fps) * spd) *
            rate_));
        const auto wit = feed_watermarks_.find(clip.id);
        const bool have_wm = wit != feed_watermarks_.end();
        const int64_t wm = have_wm ? wit->second : start_sample;
        const int64_t from = have_wm ? std::max(start_sample, wm) : start_sample;
        const int64_t want_media = canvas::core::cliprate::media_span_for_output(clip, want_frames);
        const int64_t span = start_sample + want_media - from;
        if (cut_diag_ > 0 && span <= 0)
            CANVAS_LOG(
                "[diag:cut]   STALE clip=%lld media=%d start=%lld wm=%lld span=%lld (write skip)",
                static_cast<long long>(clip.id), clip.media, static_cast<long long>(start_sample),
                static_cast<long long>(have_wm ? wit->second : start_sample),
                static_cast<long long>(span));
        if (span <= 0) continue;

        auto ait = decoders_.find(clip.media);
        if (ait == decoders_.end() || !ait->second->has_audio()) continue;
        const auto dec_t0 = Clock::now();
        auto chunk = ait->second->decode(from, static_cast<int>(std::min<int64_t>(span, want_media)),
                                         rate_);
        const double dec_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - dec_t0).count();
        step_decode_ms_ += dec_ms;
        if (!chunk || chunk->samples.empty()) continue;
        if (playback_dbg() || cut_diag_ > 0) {
            const int dbg_ch = chunk->channels > 0 ? chunk->channels : 1;
            const int dbg_frames = static_cast<int>(chunk->samples.size()) / dbg_ch;
            CANVAS_LOG("[audio] DECODE media=%d from=%lld span=%lld dec_ms=%.2f frames=%d",
                       clip.media, static_cast<long long>(from),
                       static_cast<long long>(span), dec_ms, dbg_frames);
        }
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
            CANVAS_LOG(
                "[diag:cut]   SRC clip=%lld media=%d start=%lld from=%lld span=%lld frames=%d "
                "wm=%lld src_out_sample=%lld",
                static_cast<long long>(clip.id), clip.media, static_cast<long long>(start_sample),
                static_cast<long long>(from), static_cast<long long>(span), frames,
                static_cast<long long>(feed_watermarks_[clip.id]),
                static_cast<long long>(src_out_sample));

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
            if (written <= 0) continue;
            pcm = sped.data();
            den_frames = written;
            den_ch = out_ch;
        }

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
            std::vector<float> eqd;
            if (clip.eq_enabled || eq_bank_.wants_samples(clip.id, false)) {
                eqd.assign(pcm, pcm + static_cast<std::size_t>(den_frames) * den_ch);
                den_frames = eq_bank_.tick(clip.id, clip.eq_bands, clip.eq_enabled, rate_,
                                           den_ch, eqd.data(), den_frames);
                pcm = eqd.data();
            } else {
                (void)eq_bank_.tick(clip.id, clip.eq_bands, false, rate_, den_ch, nullptr, 0);
            }
        }
        if (pcm && den_frames > 0) {
            mix_source_samples(mix, channels_, clip, src.gain_db,
                               effective_clip_volume_db(clip), pcm, den_ch, den_frames, from,
                               rate_, src.fps, seq_fps, spd, 0.0f);
        }
        out_frames = std::max(out_frames, static_cast<int64_t>(den_frames));
    }
    step_mix_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - mix_t0).count();

    if (out_frames <= 0) return 0;
    const auto write_t0 = Clock::now();
    sink_.write_float(mix.data(), static_cast<int>(out_frames));
    step_write_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - write_t0).count();
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

int64_t AudioPipeline::playhead_to_audio_sample(int64_t seq_frame) const {
    if (!project_ || seq_frame < 0) return 0;
    const canvas::core::Clip* clip = audio_clip_at(seq_frame);
    if (!clip || clip->media < 0) return 0;
    const double fps_v = media_fps_of(*project_, *clip);
    if (fps_v <= 0.0) return 0;
    const double seq_fps = project_->sequence.fps;
    if (seq_fps <= 0.0) return 0;
    const double spd = canvas::core::cliprate::effective_rate(*clip);
    return static_cast<int64_t>(std::llround(
        (static_cast<double>(clip->src_in) / fps_v +
         (static_cast<double>(seq_frame - clip->tl_in) / seq_fps) * spd) *
        rate_));
}

int64_t AudioPipeline::audio_sample_to_seq_frame(int64_t media_sample) const {
    if (!project_ || media_sample < 0) return -1;
    const canvas::core::Sequence& seq = project_->sequence;
    for (const canvas::core::Track& t : seq.audio_tracks) {
        for (const canvas::core::Clip& c : t.clips) {
            if (c.media < 0 || !c.enabled) continue;
            const double fps_v = media_fps_of(*project_, c);
            if (fps_v <= 0.0) continue;
            const double seq_fps = project_->sequence.fps;
            if (seq_fps <= 0.0) continue;
            const double secs_in =
                static_cast<double>(media_sample) / rate_ - static_cast<double>(c.src_in) / fps_v;
            if (secs_in < 0.0 ||
                secs_in >= static_cast<double>(c.src_out - c.src_in) / fps_v)
                continue;
            const double spd = canvas::core::cliprate::effective_rate(c);
            const int64_t seq_frame =
                c.tl_in + static_cast<int64_t>(std::llround(secs_in / spd * seq_fps));
            return seq_frame;
        }
    }
    return -1;
}

void AudioPipeline::log_av_sync(int64_t seq_frame, double video_fps, double step_seconds) {
    static auto last_av = Clock::now();
    const auto now = Clock::now();
    if (now - last_av < std::chrono::seconds(1)) return;
    last_av = now;

    const double video_ms = seq_frame / video_fps * 1000.0;
    const uint64_t audible_frames = sink_.audible_position_frames();
    const int64_t run_audible =
        static_cast<int64_t>(audible_frames) - static_cast<int64_t>(written_at_anchor_);
    const double audible_ms =
        static_cast<double>(anchor_seq_frame_) / video_fps * 1000.0 +
        static_cast<double>(run_audible) / static_cast<double>(rate_) * 1000.0;

    const double written_ms =
        static_cast<double>(anchor_media_sample_ +
                            static_cast<int64_t>(sink_.stat_written_frames()) -
                            static_cast<int64_t>(written_at_anchor_)) /
        static_cast<double>(rate_) * 1000.0;

    double speed_ratio = 0.0;
    if (speed_run_ == run_id_ && speed_video_ms_ > 0.0) {
        const double dv = video_ms - speed_video_ms_;
        const double da = audible_ms - speed_audible_ms_;
        if (dv > 1.0 && da >= 0.0) speed_ratio = da / dv;
    }
    speed_run_ = run_id_;
    speed_video_ms_ = video_ms;
    speed_audible_ms_ = audible_ms;

    ::canvas::core::log::log_audio_info(
        "[avsync] run=%llu frame=%lld video_ms=%.1f audible_ms=%.1f av_offset_ms=%.1f "
        "speed_x=%.3f write_to_audible_ms=%.1f step_s=%.4f",
        static_cast<unsigned long long>(run_id_), static_cast<long long>(seq_frame), video_ms,
        audible_ms, video_ms - audible_ms, speed_ratio, written_ms - audible_ms, step_seconds);
}

void AudioPipeline::play_step(int64_t seq_frame, double step_seconds, bool seek_hold_active) {
    std::lock_guard lock(mutex_);
    step_decode_ms_ = 0.0;
    step_mix_ms_ = 0.0;
    step_write_ms_ = 0.0;
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
    if (seek_hold_active) {
        warn("seek-hold active (audio waits for its frame)");
        return;
    }
    const auto sources = audible_sources_at(seq_frame);
    if (sources.empty()) {
        const canvas::core::Clip* present = clip_at_any_track(seq_frame);
        if (present && present->media >= 0) {
            static auto last_gg = Clock::now();
            static int gg_ = 0, gg_suppressed_ = 0;
            if ((++gg_) == 1 || Clock::now() - last_gg >= std::chrono::seconds(1)) {
                last_gg = Clock::now();
                if (gg_suppressed_)
                    ::canvas::core::log::log_audio_info(
                        "audio: GHOST-GUARD frame=%lld enabled=%d media=%d tl=%lld->%lld suppressed=%d",
                        static_cast<long long>(seq_frame), (int)present->enabled, present->media,
                        static_cast<long long>(present->tl_in),
                        static_cast<long long>(present->tl_out), gg_suppressed_);
                else
                    ::canvas::core::log::log_audio_info(
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
    log_av_sync(seq_frame, seq_fps, step_seconds);

    const auto wit = feed_watermarks_.find(clip->id);
    const int64_t from =
        wit != feed_watermarks_.end() ? std::max(start_sample, wit->second) : start_sample;

    if (last_drift_anchor_.time_since_epoch().count() == 0 ||
        Clock::now() - last_drift_anchor_ >= std::chrono::seconds(1)) {
        const int64_t run_audible =
            static_cast<int64_t>(sink_.audible_position_frames()) -
            static_cast<int64_t>(written_at_anchor_);
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

    const bool backward_jump = last_seq_fed_ >= 0 && seq_frame < last_seq_fed_ - 1;
    constexpr int64_t kForwardReanchorSamples = 48000;
    const bool forward_hop =
        last_seq_fed_ >= 0 && start_sample - from > kForwardReanchorSamples;
    if (backward_jump || forward_hop) {
        ::canvas::core::log::log_audio_warning(
            "audio: playhead discontinuity at frame=%lld (last_fed=%lld %s) — self re-anchor",
            static_cast<long long>(seq_frame), static_cast<long long>(last_seq_fed_),
            backward_jump ? "backward" : "forward-hop");
        reanchor_locked(seq_frame);
    }

    const int64_t backlog_f = static_cast<int64_t>(sink_.stat_written_frames()) -
                              static_cast<int64_t>(sink_.audible_position_frames());
    const int64_t lead_f = static_cast<int64_t>(
        static_cast<double>(rate_) * kAudioLeadMs / 1000.0);
    const bool deferred = backlog_f > lead_f + want;
    int64_t written = deferred ? 0 : write_mixed(seq_frame, want);
    last_seq_fed_ = std::max(last_seq_fed_, seq_frame);
    if (!deferred && written <= 0 && backlog_f < lead_f) {
        const int64_t deficit = lead_f - backlog_f;
        const int64_t extra = std::min<int64_t>(deficit, want);
        written = write_mixed(seq_frame, want + static_cast<int64_t>(extra));
    }
    {
        const bool clip_changed = clip->id != last_primary_clip_;
        last_primary_clip_ = clip->id;
        const bool near_start = seq_frame < clip->tl_in + 4;
        const bool near_end = clip->tl_out > 0 && seq_frame > clip->tl_out - 4;
        if (clip_changed || near_start || near_end) cut_diag_ = 40;
        if (clip_changed)
            ::canvas::core::log::log_audio_info(
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
            CANVAS_LOG(
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

    static auto last_afe_log = Clock::now();
    static int afe_ = 0;
    static uint64_t last_resync_total = 0;
    if ((++afe_) == 1 || Clock::now() - last_afe_log >= std::chrono::seconds(1)) {
        last_afe_log = Clock::now();
        static int64_t last_start = 0;
        uint64_t resync_total = 0;
        for (const auto& [id, adec] : decoders_) resync_total += adec->resync_count();
        const uint64_t resync_delta = resync_total - last_resync_total;
        const int64_t written_now = written;
        const bool seek_hop = last_start != 0 && std::llabs(from - last_start) > want * 4;
        const bool burst = written_now > want * 4;
        last_start = from;
        const uint64_t audible_frames = sink_.audible_position_frames();
        const int64_t run_audible =
            static_cast<int64_t>(audible_frames) - static_cast<int64_t>(written_at_anchor_);
        const double audible_ms =
            static_cast<double>(anchor_seq_frame_) / seq_fps * 1000.0 +
            static_cast<double>(run_audible) / static_cast<double>(rate_) * 1000.0;
        const double video_ms = static_cast<double>(seq_frame) / seq_fps * 1000.0;
        ::canvas::core::log::log_audio_info(
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

void AudioPipeline::feed_scrub_audio(int64_t target) {
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
    const int64_t base_sample = static_cast<int64_t>(std::llround(
        (static_cast<double>(clip->src_in) / fps_v +
         static_cast<double>(target - clip->tl_in) / seq_fps) *
        rate_));
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
        mix_source_chunk(chunk_mix, channels_, *clip, sources[0].gain_db,
                         effective_clip_volume_db(*clip), s, base_sample, rate_, fps_v,
                         seq_fps, 1.0, clip->pan);
        ok = sink_.reposition_enqueue(chunk_mix.data(), frames);
    }
    const std::size_t pending_after = sink_.pending_frames();
    if (ok) ++scrub_repositions_;
    last_scrub_audio_target_ = target;
    const double target_ms = static_cast<double>(target) / seq_fps * 1000.0;
    const double chunk_ms = static_cast<double>(frames) / rate_ * 1000.0;
    CANVAS_LOG(
        "[scrub] FEED target=%lld target_ms=%.0f base_sample=%lld chunk_ms=%.0f frames=%d "
        "pending_before=%zu pending_after=%zu%s",
        static_cast<long long>(target), target_ms, static_cast<long long>(base_sample), chunk_ms,
        frames, pending_before, pending_after, ok ? "" : " DROPPED");
}

void AudioPipeline::begin_scrub() {
    std::lock_guard lock(mutex_);
    last_scrub_audio_target_ = -1;
    last_scrub_audio_at_ = {};
    scrub_repositions_ = 0;
}

int64_t AudioPipeline::audible_seq_frame(int64_t playhead_seq) const {
    std::lock_guard lock(mutex_);
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

}
