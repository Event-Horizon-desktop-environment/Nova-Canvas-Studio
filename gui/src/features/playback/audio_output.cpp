#include "audio_output.hpp"
#include "Logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <cmath>

// PipeWire
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>

// ALSA
#include <alsa/asoundlib.h>

namespace canvas::gui {

namespace {
// Cap the PCM queued for the PipeWire callback before we start dropping, so the
// process thread can't run unboundedly ahead of the graph or stall the decode
// worker in write_float().
constexpr std::size_t kPwQueuedLimit = 48000 * 4;  // 1s at 48kHz mono-frames

void default_channel_map(int channels, uint32_t* map) {
    static constexpr uint32_t kStereo[2] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
    static constexpr uint32_t kMono[1] = {SPA_AUDIO_CHANNEL_MONO};
    if (channels == 2 && map) {
        std::memcpy(map, kStereo, sizeof(kStereo));
    } else if (channels == 1 && map) {
        std::memcpy(map, kMono, sizeof(kMono));
    } else if (map) {
        for (int i = 0; i < channels && i < static_cast<int>(SPA_AUDIO_MAX_CHANNELS); ++i)
            map[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
    }
}
}  // namespace

struct AudioOutput::Impl {
    int rate = 0;
    int channels = 0;

    // ---- PipeWire ----
    pw_main_loop* loop = nullptr;
    pw_context* ctx = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    bool pw_ok = false;
    std::thread pw_thread_;
    bool pw_stopping_ = false;

    std::mutex q_mutex;
    std::vector<float> q;         // interleaved float samples
    std::size_t q_start = 0;      // index of first unconsumed sample

    // ---- ALSA ----
    snd_pcm_t* pcm = nullptr;
    bool alsa_ok = false;
    std::thread alsa_thread_;
    std::condition_variable q_cv;
    bool alsa_stopping_ = false;
    std::size_t q_max = 0;  // max queued samples (bounding latency)
    // When true the writer feeds silence-between real chunks so the device never
    // underruns during live playback (see set_hold_active).
    std::atomic<bool> hold_active{false};
    // Generation counter: bumped by every reposition_enqueue(). The writer
    // captures it per batch and aborts if it changed mid-write, so audio from an
    // old position can't be pushed through a freshly-repositioned device.
    std::atomic<uint64_t> gen{0};

    // Always-on pipeline accounting (see AudioOutput stat getters).
    std::atomic<uint64_t> stat_enqueued_frames{0};
    std::atomic<uint64_t> stat_dropped_frames{0};
    std::atomic<uint64_t> stat_written_frames{0};
    std::atomic<uint64_t> stat_write_errors{0};
    // Live-playback silence holds fed by the writer (holding the device open
    // between real chunks) and XRUN recoveries — the audible pop signature. A
    // silence hold that outpaces real data means the decode/feed is starving the
    // device; XRUNs mean we fell far enough behind to corrupt the stream.
    std::atomic<uint64_t> stat_silence_holds{0};
    std::atomic<uint64_t> stat_silence_hold_frames{0};
    std::atomic<uint64_t> stat_xruns{0};

    static void on_process(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (!self || !self->stream) return;

        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b) return;

        spa_buffer* buf = b->buffer;
        if (buf->n_datas < 1) {
            if (playback_debug())
                qDebug() << "audio: PW on_process no datas";
            pw_stream_queue_buffer(self->stream, b);
            return;
        }
        spa_data* d = &buf->datas[0];
        if (!d->data || d->maxsize == 0) {
            if (playback_debug())
                qDebug() << "audio: PW on_process no data/maxsize=" << d->maxsize;
            pw_stream_queue_buffer(self->stream, b);
            return;
        }

        std::size_t bytes_wanted = 0;
        float* src = nullptr;
        std::size_t src_len = 0;
        {
            std::lock_guard<std::mutex> lock(self->q_mutex);
            if (self->q_start >= self->q.size()) {
                self->q.clear();
                self->q_start = 0;
            }
            src = self->q.data() + self->q_start;
            src_len = self->q.size() - self->q_start;
        }

        // The device provides one interleaved plane; fill as much as fits.
        std::size_t avail_bytes = static_cast<std::size_t>(d->maxsize);
        avail_bytes = (avail_bytes / 16) * 16;  // keep alignment sane
        const std::size_t wanted_samples = std::min(src_len, avail_bytes / sizeof(float));
        std::size_t written = 0;
        if (wanted_samples > 0) {
            std::memcpy(d->data, src, wanted_samples * sizeof(float));
            written = wanted_samples;
        }
        b->size = written / static_cast<std::size_t>(self->channels);  // frames

        if (written > 0) {
            std::lock_guard<std::mutex> lock(self->q_mutex);
            self->q_start += written;
            if (self->q_start == self->q.size()) {
                self->q.clear();
                self->q_start = 0;
            }
        }

        if (playback_debug()) {
            static unsigned pw_dbg_ = 0;
            if ((pw_dbg_++ & 63u) == 0u)
                qDebug() << "audio: PW on_process wanted_samples=" << wanted_samples
                         << "written_samples=" << written << "queued_after="
                         << (self->q.size() - self->q_start);
        }

        pw_stream_queue_buffer(self->stream, b);
    }

    static const pw_stream_events stream_events() {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = on_process;
        return e;
    }

    // Dedicated ALSA writer: the decode/present worker never blocks on the
    // realtime-paced device, it only pushes into the queue and this thread drains
    // it (snd_pcm_writei on the worker would gate video pacing to ~30Hz).
    static void alsa_worker(Impl* self) {
        // Throttle verbose writer logs to ~1/s.
        auto last_log = std::chrono::steady_clock::now();
        uint64_t wrote_batch = 0, errs_batch = 0;
        // Pause-hold was tried but on `default` (PipeWire/PulseAudio plugin) devices
        // snd_pcm_pause is unreliable and leaves the device frozen. Instead a short
        // silence hold keeps it RUNNING (no XRUN) with the clock advancing in step
        // with the writer, so audio resumes at the right place.
        const std::size_t silence_ch = static_cast<std::size_t>(self->channels);
        const std::size_t silence_hold =
            silence_ch * (static_cast<std::size_t>(self->rate) / 100);  // ~10ms per feed
        std::vector<float> silence(silence_hold, 0.0f);
        for (;;) {
            std::vector<float> to_write;
            bool starved = false;
            {
                std::unique_lock<std::mutex> lock(self->q_mutex);
                if (!self->alsa_stopping_ && self->q_start >= self->q.size()) {
                    if (!self->hold_active.load(std::memory_order_relaxed)) {
                        // Idle (paused): block until audio arrives or stop is set —
                        // don't spin feeding silence.
                        self->q_cv.wait(lock, [self] {
                            return self->alsa_stopping_ ||
                                   self->hold_active.load(std::memory_order_relaxed) ||
                                   (self->q_start < self->q.size());
                        });
                        if (self->alsa_stopping_) break;
                        if (self->q_start < self->q.size()) {
                            to_write.assign(self->q.begin() + self->q_start, self->q.end());
                            self->q.clear();
                            self->q_start = 0;
                        } else {
                            starved = true;  // hold active + empty queue -> silence-hold
                        }
                    } else {
                        // Live playback, no pending audio: brief silence hold so
                        // the device stays running.
                        starved = true;
                    }
                } else if (!self->alsa_stopping_) {
                    to_write.assign(self->q.begin() + self->q_start, self->q.end());
                    self->q.clear();
                    self->q_start = 0;
                }
            }
            if (self->alsa_stopping_) break;
            if (starved) {
                // No audio pending and held live: write a short silence chunk to
                // keep the device running and underrun-free (never pause/freeze).
                if (self->pcm && silence_ch > 0) {
                    const snd_pcm_sframes_t want =
                        static_cast<snd_pcm_sframes_t>(silence.size() / silence_ch);
                    const snd_pcm_sframes_t err =
                        snd_pcm_writei(self->pcm, silence.data(), want);
                    if (err < 0 && playback_debug())
                        qDebug() << "audio: ALSA silence-hold writei err=" << err
                                 << "state=" << snd_pcm_state(self->pcm);
                    self->stat_written_frames.fetch_add(
                        err > 0 ? static_cast<uint64_t>(err) : 0,
                        std::memory_order_relaxed);
                    self->stat_silence_holds.fetch_add(1, std::memory_order_relaxed);
                    if (err > 0)
                        self->stat_silence_hold_frames.fetch_add(
                            static_cast<uint64_t>(err), std::memory_order_relaxed);
                }
                // Non-blocking device: give the PCM time between retries.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (to_write.empty()) continue;

            std::size_t idx = 0;
            const std::size_t total = to_write.size();
            // Capture the generation this batch belongs to; if a scrub reposition bumps
            // it while we write, this batch is stale and must be discarded.
            const uint64_t batch_gen = self->gen.load(std::memory_order_relaxed);
            while (idx < total && !self->alsa_stopping_) {
                if (self->gen.load(std::memory_order_relaxed) != batch_gen) {
                    // Reposition mid-write: always logged so stale audio is visible.
                    qWarning() << "audio: writer DISCARDED stale batch "
                                  "(repositioned mid-write) discarded_frames="
                               << ((total - idx) / static_cast<std::size_t>(self->channels));
                    break;
                }
                const snd_pcm_sframes_t frames = static_cast<snd_pcm_sframes_t>(
                    (total - idx) / static_cast<std::size_t>(self->channels));
                if (frames <= 0) break;
                snd_pcm_sframes_t err =
                    snd_pcm_writei(self->pcm, to_write.data() + idx, frames);
                if (err < 0) {
                    self->stat_write_errors.fetch_add(1, std::memory_order_relaxed);
                    ++errs_batch;
                    // -EAGAIN means "buffer full, try again"; sleep briefly rather
                    // than hot-spinning, then re-check the generation.
                    if (err == -EAGAIN) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    // A stale -EPIPE after a reposition is expected: drop it instead of
                    // streaming through the new position's device.
                    if (self->gen.load(std::memory_order_relaxed) != batch_gen) {
                        // Reposition-critical: always logged.
                        qWarning() << "audio: writer aborted batch on reposition (err="
                                   << err << ") discarded_frames="
                                   << ((total - idx) / static_cast<std::size_t>(self->channels));
                        break;
                    }
                    qWarning() << "audio: ALSA writei error" << err
                               << "(" << snd_strerror(static_cast<int>(err)) << ")"
                               << "state=" << snd_pcm_state(self->pcm);
                    self->stat_xruns.fetch_add(1, std::memory_order_relaxed);
                    if (snd_pcm_recover(self->pcm, static_cast<int>(err), 1) < 0) {
                        qWarning() << "audio: ALSA recover FAILED, stopping writer";
                        break;
                    }
                    continue;
                }
                if (err == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                idx += static_cast<std::size_t>(err) * static_cast<std::size_t>(self->channels);
                self->stat_written_frames.fetch_add(
                    static_cast<uint64_t>(err), std::memory_order_relaxed);
                wrote_batch += static_cast<uint64_t>(err);
            }

            const auto now = std::chrono::steady_clock::now();
            static uint64_t s_prev_xruns = 0;
            static uint64_t s_prev_sil = 0;
            if (playback_debug() && now - last_log >= std::chrono::seconds(1)) {
                last_log = now;
                // Device buffer-depth telemetry: the ALSA `delay` (frames still
                // queued in the device/plugin) directly measures how far ahead of
                // the audible position we are feeding. min/avg/max per second
                // show whether the pipeline rides a healthy short cushion or
                // drifts deep ahead (which hides as extra latency on scrub/seek).
                long long buf_min = 0, buf_avg = 0, buf_max = 0;
                {
                    snd_pcm_sframes_t d = 0;
                    long long acc = 0, cnt = 0, mx = 0, mn = 0;
                    bool ok = false;
                    for (int k = 0; k < 5; ++k) {
                        if (snd_pcm_delay(self->pcm, &d) == 0 && d >= 0) {
                            const long long v = static_cast<long long>(d);
                            if (!ok) { mn = mx = v; ok = true; }
                            else if (v < mn) mn = v;
                            if (v > mx) mx = v;
                            acc += v; ++cnt;
                        }
                    }
                    if (ok) {
                        buf_min = mn;
                        buf_avg = acc / cnt;
                        buf_max = mx;
                    }
                }
                qDebug() << "audio: writer wrote_frames=" << wrote_batch
                         << "errors=" << errs_batch
                         << "cum_written=" << self->stat_written_frames.load()
                         << "cum_enqueued=" << self->stat_enqueued_frames.load()
                         << "cum_dropped=" << self->stat_dropped_frames.load()
                         << "queue_pending="
                         << (self->q.size() - self->q_start)
                         << "pcm_state=" << snd_pcm_state(self->pcm)
                         << "buf_frames=" << buf_min << "/" << buf_avg << "/" << buf_max
                         << "xruns_delta=" << (self->stat_xruns.load(std::memory_order_relaxed) - s_prev_xruns)
                         << "silence_frames="
                         << (self->stat_silence_hold_frames.load(std::memory_order_relaxed) - s_prev_sil);
                s_prev_xruns = self->stat_xruns.load(std::memory_order_relaxed);
                s_prev_sil = self->stat_silence_hold_frames.load(std::memory_order_relaxed);
                wrote_batch = 0;
                errs_batch = 0;
            }
        }
    }

    void alsa_thread_start() {
        if (alsa_ok && !alsa_thread_.joinable()) {
            alsa_stopping_ = false;
            alsa_thread_ = std::thread(alsa_worker, this);
            if (debug_enabled()) qDebug() << "audio: ALSA writer thread started";
        }
    }

    void alsa_thread_stop() {
        if (alsa_thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(q_mutex);
                alsa_stopping_ = true;
            }
            q_cv.notify_all();
            alsa_thread_.join();
            {
                std::lock_guard<std::mutex> lock(q_mutex);
                alsa_stopping_ = false;
            }
        }
    }

    void pw_start() {
        if (!pw_thread_.joinable() && loop) {
            pw_thread_ = std::thread([](pw_main_loop* l) { pw_main_loop_run(l); }, loop);
        }
    }

    void pw_stop() {
        if (pw_thread_.joinable()) {
            if (loop) pw_main_loop_quit(loop);
            pw_thread_.join();
        }
    }

    void destroy_pw() {
        pw_stop();
        if (stream) {
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
        if (ctx) {
            pw_context_destroy(ctx);
            ctx = nullptr;
        }
        if (loop) {
            pw_main_loop_destroy(loop);
            loop = nullptr;
        }
    }

    ~Impl() {
        alsa_thread_stop();
        if (pcm) {
            snd_pcm_drain(pcm);
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
        destroy_pw();
        if (pw_init_done_) pw_deinit();
    }
    bool pw_init_done_ = false;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() = default;

bool AudioOutput::open(const int sample_rate, const int channels) {
    close();
    if (sample_rate <= 0 || channels <= 0) return false;

    impl_->rate = sample_rate;
    impl_->channels = channels;

    if (debug_enabled())
        qDebug() << "audio: open requested rate=" << sample_rate << "channels=" << channels;

    // ---- Prefer ALSA (blocking writei gives reliable, realtime-paced playback) ----
    int err = snd_pcm_open(&impl_->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        // Always-on: only shows when ALSA is genuinely absent/unusable, and that
        // is exactly when a user can't hear output ("why is audio silent?") —
        // capturing the precise reason here beats a blank fallback.
        qWarning() << "[audio] ALSA open FAILED"
                   << "(" << snd_strerror(err) << ")"
                   << "-> falling back to PipeWire";
        impl_->pcm = nullptr;
    } else {
        snd_pcm_nonblock(impl_->pcm, 1);
        err = snd_pcm_set_params(impl_->pcm, SND_PCM_FORMAT_FLOAT_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED, channels, sample_rate, 1,
                                 50000 /* ~50ms buffer */);
        if (err < 0) {
            qWarning() << "[audio] ALSA set_params FAILED"
                       << "(" << snd_strerror(err) << ")"
                       << "rate=" << sample_rate << "channels=" << channels
                       << "-> falling back to PipeWire";
            snd_pcm_close(impl_->pcm);
            impl_->pcm = nullptr;
        } else {
            impl_->alsa_ok = true;
            open_ = true;
            impl_->q_max = static_cast<std::size_t>(sample_rate) * 3;  // ~64ms
            impl_->alsa_thread_start();
            qWarning() << "[audio] ALSA OPENED rate=" << sample_rate
                       << "channels=" << channels;
            return true;
        }
    }

    // ---- Fall back to PipeWire ----
    pw_init(nullptr, nullptr);
    impl_->pw_init_done_ = true;

    impl_->loop = pw_main_loop_new(nullptr);
    if (impl_->loop) {
        pw_loop* pl = pw_main_loop_get_loop(impl_->loop);
        impl_->ctx = pw_context_new(pl, nullptr, 0);
        if (impl_->ctx) {
            impl_->core = pw_context_connect(impl_->ctx, nullptr, 0);
            if (impl_->core) {
                const auto events = Impl::stream_events();
                impl_->stream =
                    pw_stream_new_simple(pl, "canvas-audio",
                                         pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                                           PW_KEY_MEDIA_CATEGORY, "Playback",
                                                           PW_KEY_NODE_NAME, "canvas-audio",
                                                           nullptr),
                                         &events, impl_.get());
                if (impl_->stream) {
                    const int n_channels = channels;
                    spa_audio_info_raw info{};
                    info.format = SPA_AUDIO_FORMAT_F32;
                    info.rate = static_cast<uint32_t>(sample_rate);
                    info.flags = SPA_AUDIO_FLAG_NONE;
                    info.channels = static_cast<uint32_t>(n_channels);
                    default_channel_map(n_channels, info.position);

                    uint8_t buffer[1024];
                    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                    const spa_pod* params[1];
                    params[0] = reinterpret_cast<const spa_pod*>(
                        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info));

            if (params[0] &&
                        pw_stream_connect(impl_->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                          static_cast<enum pw_stream_flags>(
                                              PW_STREAM_FLAG_AUTOCONNECT),
                                          params, 1) == 0) {
                        impl_->pw_ok = true;
                        // Run the loop so the process callback drains our queue.
                        impl_->pw_start();
                    } else if (impl_->stream) {
                        if (debug_enabled())
                            qDebug() << "audio: PipeWire stream_connect FAILED";
                        pw_stream_destroy(impl_->stream);
                        impl_->stream = nullptr;
                    }
                } else {
                    pw_core_disconnect(impl_->core);
                    impl_->core = nullptr;
                }
            } else {
                pw_context_destroy(impl_->ctx);
                impl_->ctx = nullptr;
            }
        }
    }

    if (impl_->pw_ok) {
        open_ = true;
        qWarning() << "[audio] PIPEWIRE OPENED rate=" << sample_rate
                   << "channels=" << channels;
        return true;
    }

    qWarning() << "[audio] OPEN FAILED (ALSA + PipeWire) rate=" << sample_rate
               << "channels=" << channels;
    return false;
}

void AudioOutput::close() {
    if (debug_enabled()) qDebug() << "audio: close";
    if (impl_) {
        impl_->alsa_thread_stop();
        if (impl_->pcm) {
            snd_pcm_drop(impl_->pcm);
            snd_pcm_close(impl_->pcm);
            impl_->pcm = nullptr;
        }
        impl_->alsa_ok = false;
        impl_->destroy_pw();
        impl_->pw_ok = false;
        {
            std::lock_guard<std::mutex> lock(impl_->q_mutex);
            impl_->q.clear();
            impl_->q_start = 0;
        }
    }
    open_ = false;
    if (debug_enabled())
        qDebug() << "audio: close done"
                 << "enqueued=" << stat_enqueued_frames()
                 << "written=" << stat_written_frames()
                 << "dropped=" << stat_dropped_frames();
}

bool AudioOutput::write_float(const float* data, const int frames) {
    if (!open_ || frames <= 0 || !data) {
        qWarning() << "audio: write_float called but not open / bad args open=" << open_
                   << "frames=" << frames;
        return false;
    }
    const int ch = impl_->channels;
    const std::size_t n = static_cast<std::size_t>(frames) * ch;

    // Monitoring volume: scale every sample toward the current gain. Unscaled data
    // passes through with no copy.
    const float gain = effective_volume();
    std::vector<float> scaled;
    const float* src = data;
    if (gain < 1.0f || gain > 1.0f) {
        scaled.resize(n);
        for (std::size_t i = 0; i < n; ++i) scaled[i] = data[i] * gain;
        src = scaled.data();
    }

    if (impl_->alsa_ok && impl_->pcm) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(impl_->q_mutex);
            const std::size_t buffered = impl_->q.size() - impl_->q_start;
            if (impl_->q_max > 0 && buffered + n > impl_->q_max) {
                dropped = true;
                impl_->stat_dropped_frames.fetch_add(
                    static_cast<uint64_t>(buffered / static_cast<std::size_t>(ch) +
                                          n / static_cast<std::size_t>(ch)),
                    std::memory_order_relaxed);
// Bound latency: drop oldest to make room rather than blocking the worker on
                // the realtime-paced device.
                impl_->q.clear();
                impl_->q_start = 0;
            }
            impl_->q.insert(impl_->q.end(), src, src + n);
            impl_->stat_enqueued_frames.fetch_add(static_cast<uint64_t>(frames),
                                                  std::memory_order_relaxed);
            impl_->q_cv.notify_all();
        }
        if (dropped)
            qWarning() << "audio: ALSA queue overflow, dropped buffered samples; "
                       << "enqueued=" << impl_->stat_enqueued_frames.load()
                       << "written=" << impl_->stat_written_frames.load()
                       << "dropped=" << impl_->stat_dropped_frames.load();
        return true;
    }

    if (impl_->pw_ok && impl_->stream) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(impl_->q_mutex);
            const std::size_t buffered = impl_->q.size() - impl_->q_start;
            if (buffered + n > kPwQueuedLimit) {
                dropped = true;
                impl_->stat_dropped_frames.fetch_add(
                    static_cast<uint64_t>(buffered / static_cast<std::size_t>(ch) +
                                          n / static_cast<std::size_t>(ch)),
                    std::memory_order_relaxed);
                // Drop the oldest to make room (best-effort playback).
                impl_->q.clear();
                impl_->q_start = 0;
            }
            impl_->q.insert(impl_->q.end(), src, src + n);
            impl_->stat_enqueued_frames.fetch_add(static_cast<uint64_t>(frames),
                                                  std::memory_order_relaxed);
        }
        if (dropped)
            qWarning() << "audio: PipeWire queue overflow, dropped buffered samples";
        return true;
    }

    qWarning() << "audio: write_float with no active backend, frames=" << frames;
    return false;
}

void AudioOutput::flush() {
    if (debug_enabled()) qDebug() << "audio: flush";
    if (!open_) return;
    // Full re-arm used by the COMMIT path (seek / play / scrub release): stop +
    // join the writer, clear the queue, drop + prepare the device, restart the
    // writer. Heavy — fine at seek/release rate, too heavy per scrub-move (that's
    // reposition_enqueue()'s job).
    impl_->alsa_thread_stop();
    {
        std::lock_guard<std::mutex> lock(impl_->q_mutex);
        impl_->q.clear();
        impl_->q_start = 0;
        // Reposition semantics: future batches must start clean too.
        impl_->gen.fetch_add(1, std::memory_order_relaxed);
    }
    if (impl_->alsa_ok && impl_->pcm) {
        const int dr = snd_pcm_drop(impl_->pcm);
        const int pr = snd_pcm_prepare(impl_->pcm);
        if (dr < 0 || pr < 0)
            qWarning() << "audio: ALSA flush drop=" << dr << "prepare=" << pr
                       << "state=" << snd_pcm_state(impl_->pcm);
    }
    impl_->alsa_thread_start();
}

// Scrub reposition (cheap enough for per-mouse-move use): discard all pending
// queue audio AND device-buffered audio, then queue `data` as the next thing the
// device plays, so the audible position jumps to the new scrub target. Unlike
// flush() it does not stop/join the writer — it clears the queue, bumps the
// generation counter and drop/prepares the PCM while the writer stays alive; the
// writer drops any stale in-flight batch via the generation.
bool AudioOutput::reposition_enqueue(const float* data, const int frames) {
    if (!open_ || frames <= 0 || !data) return false;
    const int ch = impl_->channels;
    const std::size_t n = static_cast<std::size_t>(frames) * ch;

    const float gain = effective_volume();
    std::vector<float> scaled;
    const float* src = data;
    if (gain < 1.0f || gain > 1.0f) {
        scaled.resize(n);
        for (std::size_t i = 0; i < n; ++i) scaled[i] = data[i] * gain;
        src = scaled.data();
    }

    {
        std::lock_guard<std::mutex> lock(impl_->q_mutex);
        const int dr = impl_->alsa_ok && impl_->pcm ? snd_pcm_drop(impl_->pcm) : 0;
        const int pr = impl_->alsa_ok && impl_->pcm ? snd_pcm_prepare(impl_->pcm) : 0;
        impl_->q.clear();
        impl_->q_start = 0;
        impl_->gen.fetch_add(1, std::memory_order_relaxed);
        impl_->q.insert(impl_->q.end(), src, src + n);
        impl_->stat_enqueued_frames.fetch_add(static_cast<uint64_t>(frames),
                                              std::memory_order_relaxed);
        // Reposition-critical diagnostic: ALWAYS logged so the scrub-audio
        // audit can see each drop/reprepare and exact queue size.
        qWarning().nospace()
            << "audio: reposition gen=" << impl_->gen.load(std::memory_order_relaxed)
            << " drop=" << dr << " prepare=" << pr
            << " frames=" << frames
            << " queue_frames=" << (impl_->q.size() / static_cast<std::size_t>(impl_->channels))
            << " pcm_state=" << (impl_->pcm ? snd_pcm_state(impl_->pcm) : -99);
        impl_->q_cv.notify_all();
    }
    return true;
}

std::size_t AudioOutput::pending_frames() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->q_mutex);
    const std::size_t ch = impl_->channels > 0 ? static_cast<std::size_t>(impl_->channels) : 1;
    return (impl_->q.size() - impl_->q_start) / ch;
}

uint64_t AudioOutput::stat_enqueued_frames() const {
    return impl_ ? impl_->stat_enqueued_frames.load(std::memory_order_relaxed) : 0;
}
uint64_t AudioOutput::stat_dropped_frames() const {
    return impl_ ? impl_->stat_dropped_frames.load(std::memory_order_relaxed) : 0;
}
uint64_t AudioOutput::stat_written_frames() const {
    return impl_ ? impl_->stat_written_frames.load(std::memory_order_relaxed) : 0;
}

uint64_t AudioOutput::audible_position_frames() const {
    if (!impl_) return 0;
    const uint64_t written = impl_->stat_written_frames.load(std::memory_order_relaxed);
    if (written == 0) return 0;
    if (impl_->alsa_ok && impl_->pcm && impl_->rate > 0) {
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(impl_->pcm, &delay) == 0 && delay >= 0) {
            const std::size_t bufsz = static_cast<std::size_t>(delay);
            // Raw buffered-ahead frames we still hold (not yet handed to ALSA).
            std::size_t held = 0;
            {
                std::lock_guard<std::mutex> lock(impl_->q_mutex);
                held = impl_->q.size() - impl_->q_start;
            }
            const std::size_t channels = static_cast<std::size_t>(
                impl_->channels > 0 ? impl_->channels : 1);
            // held is in interleaved samples -> convert to frames
            const std::size_t held_frames = held / channels;
            const uint64_t played = written >= (bufsz + held_frames)
                ? written - bufsz - held_frames
                : 0;
            return played;
        }
        return written;
    }
    // PipeWire / fallback: best-effort, approximate with the written cursor.
    return written;
}
uint64_t AudioOutput::stat_write_errors() const {
    return impl_ ? impl_->stat_write_errors.load(std::memory_order_relaxed) : 0;
}
uint64_t AudioOutput::stat_silence_holds() const {
    return impl_ ? impl_->stat_silence_holds.load(std::memory_order_relaxed) : 0;
}
uint64_t AudioOutput::stat_silence_hold_frames() const {
    return impl_ ? impl_->stat_silence_hold_frames.load(std::memory_order_relaxed) : 0;
}
uint64_t AudioOutput::stat_xruns() const {
    return impl_ ? impl_->stat_xruns.load(std::memory_order_relaxed) : 0;
}

void AudioOutput::set_hold_active(bool on) {
    if (!impl_) return;
    if (debug_enabled())
        qDebug() << "audio: set_hold_active on=" << on
                 << "queue_pending=" << (impl_->q.size() - impl_->q_start);
    impl_->hold_active.store(on, std::memory_order_relaxed);
    if (on) {
        // Wake the writer so it starts holding the device (it may be blocked
        // waiting in the idle path).
        impl_->q_cv.notify_all();
    }
}

void AudioOutput::set_volume(float volume) {
    volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    volume_.store(volume);
    muted_.store(false);  // explicitly dragging the slider un-mutes
    if (debug_enabled())
        qDebug() << "audio: set_volume" << volume;
}

void AudioOutput::set_muted(bool muted) {
    muted_.store(muted);
    if (debug_enabled())
        qDebug() << "audio: set_muted" << muted << "(volume=" << volume_.load() << ")";
}

// Dim dips monitoring to ~-15dB (0.18x) of the slider level — loud enough to
// follow the material, low enough to talk over it.
static constexpr float kDimGain = 0.18f;

void AudioOutput::set_dimmed(bool dimmed) {
    dimmed_.store(dimmed);
    if (debug_enabled())
        qDebug() << "audio: set_dimmed" << dimmed;
}

float AudioOutput::effective_volume() const {
    const float base = muted_.load() ? 0.0f : volume_.load();
    return dimmed_.load() ? base * kDimGain : base;
}

void AudioOutput::log_pipeline_stats(const char* tag) const {
    qWarning().nospace()
        << "audio[pipeline:" << tag << "] enqueued=" << stat_enqueued_frames()
        << " written=" << stat_written_frames() << " dropped=" << stat_dropped_frames()
        << " write_errors=" << stat_write_errors()
        << " silence_holds=" << stat_silence_holds()
        << " silence_frames=" << stat_silence_hold_frames()
        << " xruns=" << stat_xruns()
        << " open=" << open_
        << " alsa=" << (impl_ && impl_->alsa_ok)
        << " pw=" << (impl_ && impl_->pw_ok && impl_->stream)
        << " pcm_state=" << (impl_ && impl_->pcm ? snd_pcm_state(impl_->pcm) : -99);
}

}
