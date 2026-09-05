#include "sequence_controller.hpp"
#include "Logging.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace canvas::gui {

using Clock = std::chrono::steady_clock;

SequenceController::SequenceController(QObject* parent) : QObject(parent), audio_(audio_out_) {
    qRegisterMetaType<std::shared_ptr<const canvas::core::VideoFrame>>();
    qRegisterMetaType<canvas::core::RenderFramePtr>();
    worker_ = std::thread([this] { worker_loop(); });
}

SequenceController::~SequenceController() {
    stopping_.store(true);
    {
        const std::lock_guard lock(mutex_);
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    audio_.reset();
    audio_out_.close();
}

void SequenceController::push(Request request) {
    {
        const std::lock_guard lock(mutex_);
        // Coalesce rapid commands so the queue does not build a backlog of
        // stale positions. A new Seek supersedes any queued Seek (only the
        // latest position matters), and a Pause supersedes any queued Play.
        // Without this, every scrub mouse-move enqueues a Seek whose blocking
        // decode keeps executing after the mouse is released.
        const auto is_seek_cmd = [](Command c) {
            return c == Command::Seek || c == Command::SeekPreview;
        };
        queue_.erase(
            std::remove_if(queue_.begin(), queue_.end(),
                           [&](const Request& r) {
                               return (is_seek_cmd(request.command) && is_seek_cmd(r.command)) ||
                                      (request.command == Command::Pause && r.command == Command::Play) ||
                                      (request.command == Command::Play && is_seek_cmd(r.command));
                           }),
            queue_.end());
        queue_.push_back(std::move(request));
        if (debug_enabled())
            qDebug() << "playback: queued cmd=" << static_cast<int>(request.command)
                     << "arg=" << request.arg << "queue_size=" << queue_.size();
    }
    cv_.notify_all();
}

void SequenceController::set_project(std::shared_ptr<const canvas::core::Project> project,
                                     const int64_t initial_frame) {
    push({Command::SetProject, initial_frame, std::move(project), {}});
}

void SequenceController::add_media(const canvas::core::MediaEntry& entry) {
    push({Command::AddMedia, 0, {}, entry});
}

void SequenceController::play() {
    play_pause_intent_.store(true);
    push({Command::Play, 0, {}, {}});
}
void SequenceController::pause() {
    play_pause_intent_.store(false);
    push({Command::Pause, 0, {}, {}});
}
void SequenceController::toggle_play_pause() {
    const bool want = !play_pause_intent_.load();
    play_pause_intent_.store(want);
    push({want ? Command::Play : Command::Pause, 0, {}, {}});
    if (debug_enabled())
        qDebug() << "playback: TOGGLE ->" << (want ? "PLAY" : "PAUSE")
                 << "(intent=" << play_pause_intent_.load()
                 << " worker_playing=" << playing_.load() << ")";
}
void SequenceController::seek(const int64_t frame_number) {
    push({Command::Seek, frame_number, {}, {}});
}
void SequenceController::seek_preview(const int64_t frame_number) {
    // Premiere-style audible scrub while playing: normal forward program audio is
    // streamed by the worker from the dragged position (see handle_seek_preview's
    // audio_.play_step call). No per-move device reposition here — the realtime
    // sink can't hard-cut sub-100ms blips cleanly (old server-buffered audio keeps
    // playing ~100ms past each drop), so the abandoned reposition/feed approach
    // was removed. The commit seek on release does the precise final re-anchor.
    push({Command::SeekPreview, frame_number, {}, {}});
}

void SequenceController::begin_scrub() {
    scrubbing_.store(true);
    if (!scrub_drag_active_) {
        scrub_drag_active_ = true;
        scrub_start_ = Clock::now();
        scrub_first_ = current_frame_.load();
        // Fresh drag: reset the pipeline's scrub-audio feed state so the first
        // move always feeds.
        audio_.begin_scrub();
        qWarning() << "[scrub] BEGIN playing=" << playing_.load()
                   << "enabled=" << scrub_audio_enabled_.load() << "frame=" << current_frame_.load();
    }
}
void SequenceController::end_scrub() {
    // A scrub may have skipped per-move audio rewinds; the caller's committed
    // seek() (on release) re-anchors audio once and shows the crisp frame.
    scrubbing_.store(false);
    const double drag_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - scrub_start_).count();
    qWarning() << "[scrub] END released_at=" << current_frame_.load()
               << "was_playing=" << playing_.load()
               << "audio_open=" << scrub_audio_open_
               << "drag_ms=" << drag_ms
               << "first=" << scrub_first_
               << "span=" << (current_frame_.load() - scrub_first_)
               << "repositions=" << audio_.repositions_since_begin()
               << "pending=" << audio_out_.pending_frames();
    scrub_drag_active_ = false;
    // Resolve the scrub-audio device. If we opened the output for audible
    // scrubbing while paused, drop it so the next Play opens unconditionally and
    // re-anchors from the new position. If we were playing and the drag left the
    // output alive, it is re-anchored by the release seek/Play path.
    scrub_audio_open_ = false;
    if (audio_.is_active() && !playing_.load()) audio_.close_output();
}
void SequenceController::step(const int64_t delta) {
    push({Command::Step, delta, {}, {}});
}

double SequenceController::fps() const { return fps_.load(); }

void SequenceController::reset_ready() {
    ready_.clear();
    ready_base_ = -1;
}

void SequenceController::fill_lookahead(const int64_t base) {
    if (!playing_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_base_ != base) {
        ready_.clear();
        ready_base_ = base;
    }
    while (ready_.size() < kLookahead) {
        const int64_t f = base + static_cast<int64_t>(ready_.size());
        if (f < 0 || f >= total_frames_.load()) break;
        auto frame = frame_for_playhead(f);
        if (!frame) break;
        ready_.push_back(std::move(frame));
    }
}

void SequenceController::worker_loop() {
    for (;;) {
        Request req;
        bool have_cmd = false;
        {
            std::unique_lock lock(mutex_);
            if (!queue_.empty()) {
                req = std::move(queue_.front());
                queue_.pop_front();
                have_cmd = true;
            }
        }
        if (have_cmd) {
            switch (req.command) {
            case Command::Stop:
                return;
            case Command::SetProject:
                handle_set_project(std::move(req.project), req.arg);
                break;
            case Command::AddMedia:
                handle_add_media(req.media);
                break;
            case Command::Play:
                handle_play();
                break;
            case Command::Pause:
                qWarning() << "[transport] PAUSE at=" << current_frame_.load();
                playing_.store(false);
                play_pause_intent_.store(false);
                if (audio_.is_active()) {
                    // Allow the device to idle; the writer stops feeding silence.
                    audio_out_.set_hold_active(false);
                    audio_out_.log_pipeline_stats("pause-flush-pre");
                    audio_out_.flush();
                    audio_out_.log_pipeline_stats("pause-flush-post");
                }
                emit playback_changed(false);
                break;
            case Command::Seek:
                audio_out_.log_pipeline_stats("seek-pre");
                handle_seek(req.arg);
                audio_out_.log_pipeline_stats("seek-post");
                break;
            case Command::SeekPreview:
                handle_seek_preview(req.arg);
                break;
            case Command::Step:
                qWarning() << "[transport] STEP delta=" << req.arg
                           << "-> target=" << (current_frame_.load() + req.arg);
                handle_seek(current_frame_.load() + req.arg);
                break;
            }
            continue;
        }

        if (playing_.load()) {
            // Decode ahead so the next present pops an already-ready frame.
            const auto t_loop = Clock::now();
            fill_lookahead(current_frame_.load() + 1);
            const auto t_filled = Clock::now();
            std::unique_lock lock(mutex_);
            const auto t_before_wait = Clock::now();
            const auto nwp = next_present_;
            const auto wat = cv_.wait_until(lock, nwp,
                           [this] { return stopping_.load() || !queue_.empty(); });
            const auto t_waited = Clock::now();
            const bool skipped = !(queue_.empty() && playing_.load());
            if (stopping_.load() && queue_.empty()) return;
            if (!skipped) {
                lock.unlock();
                present_next();
            }
            const auto t_done = Clock::now();
            if (debug_enabled()) {
                static int dt_iter_ = 0;
                if ((dt_iter_++ % 15) == 0) {
                    const double fill_ms =
                        std::chrono::duration<double, std::milli>(t_filled - t_loop).count();
                    const double wait_ms =
                        std::chrono::duration<double, std::milli>(t_waited - t_before_wait).count();
                    const double pres_ms =
                        std::chrono::duration<double, std::milli>(t_done - t_waited).count();
                    const double late_ms =
                        std::chrono::duration<double, std::milli>(t_before_wait - nwp).count();
                    qDebug() << "loop: iter fill_ms=" << fill_ms << "wait_ms=" << wait_ms
                             << "present_ms=" << pres_ms << "late_ms=" << late_ms
                             << "woke_by_cmd=" << wat
                             << "skipped=" << skipped;
                }
            }
        } else {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load() && queue_.empty()) return;
        }
    }
}

void SequenceController::handle_set_project(std::shared_ptr<const canvas::core::Project> project,
                                            const int64_t initial_frame) {
    playing_.store(false);
    play_pause_intent_.store(false);  // new project => not playing; keep button in sync
    audio_.reset();
    emit playback_changed(false);
    reset_ready();
    project_ = std::move(project);
    decoder_.close();

    if (!project_) {
        current_frame_.store(-1);
        total_frames_.store(-1);
        fps_.store(0.0);
        return;
    }

    audio_.set_project(project_.get());
    fps_.store(project_->sequence.fps);
    total_frames_.store(project_->sequence.duration_frames());
    for (const auto& m : project_->media) handle_add_media(m);
    // Preserve the playhead across edit snapshots (initial_frame < 0) so
    // unlink/blade/move/etc. do not reset the timeline to the start; only a
    // fresh open/new project requests an explicit anchor (0). handle_seek
    // clamps into range, so a stale playhead past a shrunk sequence lands at
    // the last frame rather than wrapping.
    const int64_t cur = current_frame_.load();
    const int64_t anchor = initial_frame >= 0 ? initial_frame : (cur >= 0 ? cur : 0);
    qWarning().nospace()
        << "[transport] SET-PROJECT anchor=" << anchor
        << " frames=" << total_frames_.load()
        << " fps=" << fps_.load()
        << " video_tracks=" << project_->sequence.video_tracks.size()
        << " audio_tracks=" << project_->sequence.audio_tracks.size()
        << " media=" << project_->media.size();
    handle_seek(anchor);
}

void SequenceController::handle_add_media(const canvas::core::MediaEntry& entry) {
    // Video decode slots (VideoDecoder + FrameCache + HW device) live in the
    // Qt-free TimelineDecoder; the controller only forwards the media entry.
    decoder_.add_media(entry);
    // Audio decode/feed (per-media decoders, feed watermark, audible scrub
    // grains, A/V sync anchors) lives in the Qt-free AudioPipeline.
    audio_.add_media(entry);
}

void SequenceController::handle_play() {
    if (!project_ || total_frames_.load() <= 0) return;
    play_pause_intent_.store(true);
    audio_out_.log_pipeline_stats("play-pre");
    if (current_frame_.load() >= total_frames_.load() - 1) handle_seek(0);
    reset_ready();
    audio_.open_output();
    // Always re-arm audio from the current playhead before playback starts.
    // Paused scrubbing uses seek_preview(), which deliberately skips the
    // per-move audio-rewind flush for speed; the writer/decoder can therefore
    // be left stale (or never opened) when we arrive here, so a Play must
    // reset+flush itself to guarantee audio resumes from the right sample.
    //
// audio_.rewind() calls AudioOutput::flush(), which stops+joins the ALSA
    // writer thread, drops/re-prepares the device, and starts a FRESH writer
    // thread. That fresh thread's very first loop iteration checks
    // hold_active: if it were already true (as it used to be, set below
    // *before* this call), it finds the queue still empty -- audio_.preroll()
    // hasn't run yet -- and immediately writes a silence-hold chunk to the
    // just-prepared device. Depending on how long preroll's decode takes to
    // catch up, several such chunks can land before the real preroll audio
    // does, so every Resume opened with a stray blip of silence spliced in
    // ahead of the actual audio -- audible as a click/stutter ("garbled")
    // right at the start of playback. Rewind+preroll first (queuing the real
    // lead-in audio while the writer thread is not yet holding), THEN arm the
    // hold so the writer's first real work is playing that audio, not padding
    // silence in front of it. The hold is still needed afterward, for
    // in-flight decode stalls during steady playback.
    audio_.rewind(current_frame_.load(), false);
    // Pre-fill the device with leading audio so the AUDIBLE position (written
    // minus device latency) starts aligned with the picture. Without this, audio
    // is heard a constant device-buffer latency (~70ms) behind the video.
    audio_.preroll(current_frame_.load(), kAudioLeadMs, playing_.load());
    // Live playback: keep the ALSA device topped up so it never underruns/XRUNs
    // (an XRUN-recover re-anchors the device arbitrarily → the sync drift).
    if (audio_.is_active()) audio_out_.set_hold_active(true);
    playing_.store(true);
    next_present_ = Clock::now();
    audio_out_.log_pipeline_stats("play-post");
    qWarning() << "[transport] PLAY at=" << current_frame_.load()
               << "/" << total_frames_.load();
    emit playback_changed(true);
}

void SequenceController::handle_seek(const int64_t frame_number) {
    if (!project_) return;
    int64_t target = frame_number;
    const int64_t last = total_frames_.load();
    if (last > 0) target = std::clamp(target, int64_t{0}, last - 1);
    if (target < 0) target = 0;

    reset_ready();
    // SonicSync (MLT "audio rides with its frame" model): the seek-hold gate
    // marks the window — from now until the newly decoded target frame is handed
    // to the display — during which new-position audio MUST NOT be fed. Feeding
    // it during the (slow) full-res decode below lets audio stream ahead at
    // realtime while the picture is still frozen on the old frame, leaving audio
    // hundreds of ms ahead of the new frame (the persistent offset we observed).
    // We instead: cut the old audio, decode the new frame, and only when the new
    // frame is actually presented do we close the hold and re-anchor audio to the
    // target. Audio and picture re-anchor together, exactly like MLT's purge +
    // atomic re-production.
    const bool was_playing = playing_.load();
    sonicsync_.begin_seek_hold(target);
    // current_frame_ is updated first so the audio rewind anchors the sync
    // diagnostic to the new playhead.
    current_frame_.store(target);
    // Re-anchor the pacing clock to the seek time so a subsequent present starts
    // FROM here at realtime rather than "catching up" by dropping frames owed
    // from before the seek (which would overshoot the scrub-release position).
    next_present_ = Clock::now();
    // Always-on commit diagnostics: a scrub release (or transport seek) that
    // re-anchors audio and decodes the crisp frame.
    qWarning() << "[scrub] COMMIT seek_to=" << target
               << "playing=" << was_playing
               << "scrubbing=" << scrubbing_.load();
    const auto commit_t0 = Clock::now();
    // Cut old-position audio NOW so it doesn't race forward during the decode;
    // this plays out the residual device buffer (brief, matches the still-shown
    // old frame — the MLT prefill analog) and leaves the device silent waiting on
    // the target. The per-frame feed is held by the seek-hold gate meanwhile.
    audio_.rewind(target, playing_.load());
    const double rewind_ms = std::chrono::duration<double, std::milli>(Clock::now() - commit_t0).count();
    const auto dec_t0 = Clock::now();
    auto frame = frame_for_playhead(target);
    const double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - dec_t0).count();
    if (debug_enabled())
        qDebug() << "playback: SEEK to frame" << target << "got_frame=" << (frame != nullptr);
    // --- ATOMIC RE-ANCHOR: the new frame is presented now. Audio for the target
    // may begin (and must re-anchor to the target), so close the seek-hold and
    // re-establish the audio lead so the device latency doesn't reintroduce lag.
    // This is the moment audio "rides with" the just-shown target frame (MLT).
    sonicsync_.end_seek_hold();
    emit frame_ready(std::move(frame));
    emit position_changed(target);
    const auto preroll_t0 = Clock::now();
    if (was_playing) audio_.preroll(target, kAudioLeadMs, was_playing);
    const double preroll_ms = std::chrono::duration<double, std::milli>(Clock::now() - preroll_t0).count();
    // Warm the decoded-frame cache for the frames just ahead of the playhead so
    // playback resumes instantly after a scrub (no decode-forward stall on the
    // first present). frame_for_playhead() advances the decoder and caches, so
    // re-running it for a short window makes the next fill_lookahead a cache hit.
    const auto warm_t0 = Clock::now();
    if (was_playing) warm_lookahead(target + 1);
    const double warm_ms = std::chrono::duration<double, std::milli>(Clock::now() - warm_t0).count();
    // Always-on commit timing (flush + decode + lookahead warm) for scrub latency.
    qWarning() << "[scrub] COMMIT-times rewind_ms=" << rewind_ms
               << "preroll_ms=" << preroll_ms
               << "decode_ms=" << decode_ms
               << "warm_ms=" << warm_ms
               << "total_ms=" << (rewind_ms + preroll_ms + decode_ms + warm_ms);
}

// Fast scrub preview. Decodes only the target frame at reduced resolution (a
// tiny RGBA, ~16x cheaper per GOP frame than full-res) and does not write it to
// the full-res frame cache, so it never poisons playback. Skips the expensive
// audio-rewind flush: we are paused/scrubbing, nothing is being played. The
// committed full-res frame + audio re-arm come from a later handle_seek on
// release/play.
void SequenceController::handle_seek_preview(const int64_t frame_number) {
    if (!project_) return;
    // While actually playing and NOT mid-scrub, a grab means the user repositioned
    // the playhead mid-playback; jump to the full-res, audio-aligned seek so the
    // running stream stays in step with the picture.
    if (playing_.load() && !scrubbing_.load()) {
        handle_seek(frame_number);
        return;
    }
    // Mid-scrub (playing or paused): take the fast preview path. While PLAYING the
    // audible scrub position is fed synchronously on the UI thread by seek_preview()
    // (feed_scrub_audio) — NOT here: this worker is saturated decoding preview
    // frames during a drag and would feed too late (measured scrub_audio_ms=0 on
    // every preview) or double-feed. Paused scrubbing writes grains below.
    int64_t target = frame_number;
    const int64_t last = total_frames_.load();
    if (last > 0) target = std::clamp(target, int64_t{0}, last - 1);
    if (target < 0) target = 0;
    current_frame_.store(target);

    // A newer scrub position may already be queued (the worker decodes this one
    // serially while the mouse keeps moving). If so, this request is stale: skip
    // the decode/present entirely so the playhead never shows an OLDER frame
    // AFTER a newer one (the "push forward, playhead jumps back" bug). The newer
    // queued request will present the correct latest position when it runs. (The
    // audible scrub position is not affected either way: the worker streams audio
    // for each landed position regardless of whether this video decode is skipped.)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Request& r : queue_) {
            if (r.command == Command::SeekPreview && r.arg != target) return;
        }
    }

    reset_ready();
    const auto preview_t0 = Clock::now();
    auto frame = frame_for_playhead_preview(target, kPreviewMaxDim);
    const double preview_ms = std::chrono::duration<double, std::milli>(Clock::now() - preview_t0).count();
    const bool got = frame != nullptr && (frame->nv12 || frame->a || frame->b);
    // Re-check AFTER the decode: while we were decoding, an even newer target may
    // have queued. Do not present (or move the playhead to) a frame that is no
    // longer the latest requested scrub position.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Request& r : queue_) {
            if (r.command == Command::SeekPreview && r.arg != target) return;
        }
    }
    current_frame_.store(target);
    if (debug_enabled())
        qDebug() << "playback: PREVIEW to frame" << target
                 << "got_frame=" << got;
    // MUST read pixel presence BEFORE std::move below: a moved-from RenderFramePtr
    // is null, so logging `frame->nv12/a` afterward always reports BLACK. Capture
    // the real hit now so the diagnostic is truthful.
    const int diag_nv12w = frame && frame->nv12 ? frame->nv12->width : 0;
    const int diag_aw = frame && frame->a ? std::max(frame->a->width, frame->a->height) : 0;
    const int diag_bw = frame && frame->b ? std::max(frame->b->width, frame->b->height) : 0;
    const bool diag_has_pix = frame && (frame->nv12 || frame->a || frame->b);
    emit frame_ready(std::move(frame));
    emit position_changed(target);

    // Always-on scrub preview diagnostics (qWarning so default runs capture it).
    // `hit` reports whether the frame handed to the viewer carried pixels:
    //   OK    - real picture (nv12 or rgba present)
    //   BLACK - frame emitted but with NO pixels (the renderer's black fallback),
    //           i.e. the scrub position produces a black monitor frame
    //   NONE  - a null frame (viewer holds the previous/last texture)
    // decode_ms is the full frame_for_playhead_preview cost; a high value with
    // BLACK/NONE points at a decode that ran but returned nothing usable.
    qWarning().nospace()
        << "[scrub] target=" << target << " hit=" << (diag_has_pix ? "OK" : "BLACK")
        << " nv12=" << diag_nv12w
        << " a=" << diag_aw
        << " b=" << diag_bw
        << " decode_ms=" << QString::number(preview_ms, 'f', 1)
        << " playing=" << playing_.load();

    // Paused scrub: open + re-anchor the output on the first move, then write
    // a short grain for each settled position. No competing live stream here.
    double grain_ms = 0.0;
    if (scrub_audio_enabled_.load() && !playing_.load() && !scrub_audio_open_) {
        audio_.open_output();
        if (audio_.is_active()) audio_.rewind(target, false);
        scrub_audio_open_ = true;
    }
    if (scrub_audio_enabled_.load() && audio_.is_active() && !playing_.load()) {
        const auto g0 = Clock::now();
        audio_.play_scrub_grain(target);
        grain_ms = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
    }
    // Premiere-style audible scrub while PLAYING: stream normal forward program
    // audio from the CURRENT dragged position. Letting audio_.play_step decode a
    // frame's worth at `target` means the audible content follows the scrub (audio
    // advances at decode/present rate, which during a fast drag is slower than the
    // video — so audio trails the picture, exactly Premiere's speed-scrub feel).
    // The commit seek on release re-anchors precisely. (No per-move device
    // reposition — the sink can't hard-cut sub-100ms blips; see seek_preview.)
    double scrub_audio_ms = 0.0;
    if (playing_.load() && audio_.is_active()) {
        const auto s0 = Clock::now();
        audio_.play_step(target, 1.0 / fps_.load(), sonicsync_.seek_hold_active());
        scrub_audio_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
    }

    // Precache during the drag: while still playing, if the playhead has
    // settled on the same frame (moves paused between mouse-moves, or the user
    // parks it), warm a short run of full-res frames just ahead of the target
    // so Play resumes from the ready buffer instead of a cold GOP seek. Only do
    // this when stationary — per-move warming would stall the scrub.
    double precache_ms = 0.0;
    if (playing_.load() && last_scrub_target_ == target) {
        const auto p0 = Clock::now();
        const int64_t end = std::min(target + static_cast<int64_t>(kScrubPrecache),
                                     total_frames_.load() - 1);
        for (int64_t f = std::max<int64_t>(target + 1, 0); f <= end; ++f) {
            if (!playing_.load()) break;
            frame_for_playhead(f);
        }
        precache_ms = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
    }
    last_scrub_target_ = target;

    // Emit the per-preview timing breakdown whenever any of the heavy stages ran.
    if ((scrub_log_tick_ & 3u) == 0u || grain_ms > 0.0 || precache_ms > 0.0)
        qWarning() << "[scrub] PREVIEW-times decode_ms=" << preview_ms
                   << "grain_ms=" << grain_ms
                   << "scrub_audio_ms=" << scrub_audio_ms
                   << "precache_ms=" << precache_ms
                   << "last_target=" << last_scrub_target_;
}

// Decodes up to kLookahead frames starting at `start_frame` and caches them,
// leaving the decoder positioned so immediate playback pops from cache. This
// runs on the worker thread after a seek to hide the decode-forward latency of
// resuming playback from a freshly seeked position.
//
// It is BOUNDED BY BUDGET: the worker must not linger decoding full-res frames
// here, because every millisecond blocks present_next() and starves the ALSA
// device — which is exactly what desyncs audio-video after a scrub release (the
// device runs dry / misaligns while the warm loop runs). We warm a short run but
// bail out as soon as we've spent roughly one frame interval, leaving the rest
// to be filled lazily by fill_lookahead() once playback presentation resumes.
void SequenceController::warm_lookahead(const int64_t start_frame) {
    if (!project_ || start_frame < 0) return;
    const int64_t last = total_frames_.load();
    if (last > 0 && start_frame >= last) return;
    const int64_t end = last > 0 ? std::min(start_frame + static_cast<int64_t>(kLookahead), last)
                                 : start_frame + static_cast<int64_t>(kLookahead);
    const double fps_v = fps_.load();
    const double budget_ms =
        fps_v > 0.0 ? 1100.0 / fps_v : 33.0;  // ~1.1 frame interval, then stop
    const auto t0 = Clock::now();
    for (int64_t f = start_frame; f < end; ++f) {
        if (!playing_.load()) return;
        frame_for_playhead(f);
        if (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() >= budget_ms)
            break;
    }
}

double SequenceController::media_rate_at(int64_t seq_frame) const {
    if (!project_) return fps_.load();
    return decoder_.media_rate_at(*project_, seq_frame, fps_.load());
}

void SequenceController::present_next() {
    int64_t want = current_frame_.load() + 1;
    if (want >= total_frames_.load()) {
        playing_.store(false);
        play_pause_intent_.store(false);
        emit playback_changed(false);
        return;
    }

    // === DROP-TO-REALTIME ===
    // If the worker fell far behind wall-clock (a long scrub stall, a slow
    // catch-up decode burst, or a seek/commit that re-stamped next_present_ into
    // the past), do NOT try to catch up by presenting every queued frame back to
    // back. That burst also feeds `audio_.play_step` a huge span of audio at once
    // (observed as [audio] step_ms_gap of >1e6 samples in a single ~1s log tick),
    // which the ALSA device then plays as a garbled/chopped burst -- the audible
    // "artifact / faster audio". Instead, skip the playhead forward to the frame
    // realtime expects right now and re-anchor the pacing clock, so both video
    // and audio resume at realtime with, at most, one frame/interval of delivery.
    const double rate_at_want = media_rate_at(want);
    if (rate_at_want > 0.0) {
        const auto intv_us =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate_at_want));
        const auto lateness = next_present_ == Clock::time_point{}
                                  ? Clock::duration{}
                                  : Clock::now() - next_present_;
        const auto margin = std::chrono::duration_cast<Clock::duration>(intv_us + intv_us / 4);
        if (lateness > margin) {
            // Frames of realtime we owe: advance the playhead (dropping frames)
            // so the next present lands on the realtime-expected frame.
            const int64_t owed = static_cast<int64_t>((lateness + intv_us / 2) / intv_us);
            int64_t target_catch = current_frame_.load() + owed;

            // === MASTER-CLOCK CAP (SonicSync, fixes runaway drop) ===
            // Advancing the playhead ahead of what the speaker has actually
            // consumed makes video race far ahead of what you hear (observed:
            // playhead at 376s of media while audio was still at 37s). The
            // audible position is the true realtime clock, so never let a
            // catch-up drop land more than kLookahead frames past it. When
            // decode is throughput-bound (full-res < realtime), the playhead is
            // held at audio+lead and video simply presents as fast as it decodes
            // instead of teleporting minutes ahead. SonicSync owns this policy.
            // Only trust the audible position as a master clock while there is
            // actually a live, ENABLED audio clip under the playhead. If the
            // clip is disabled/muted (no audio clip there), nothing is being
            // written to the device this run and the AudioPipeline anchor math
            // returns -1, so the playhead runs at realtime unclamped.
            int64_t aud_seq = audio_.audible_seq_frame(current_frame_.load());
            {
                const int64_t capped = sonicsync_.reconcile(
                    target_catch, aud_seq, static_cast<int64_t>(kLookahead),
                    total_frames_.load());
                if (capped != target_catch) {
                    if (playback_debug())
                        qDebug() << "playback: drop capped audible_seq=" << aud_seq
                                 << "target=" << target_catch << "->" << capped;
                    target_catch = capped;
                }
            }

            const int64_t new_frame = std::min(target_catch, total_frames_.load() - 1);
            reset_ready();
            current_frame_.store(new_frame);
            // Re-anchor the audio feed watermark to the new playhead so the next
            // play step produces audio at (and only at) realtime.
            audio_.advance_feed_for_drop(new_frame);
            // Re-anchor the pacing clock to now so following waits are non-trivial.
            next_present_ = Clock::now();
        }
    }

    // After a drop-to-realtime jump, `want` must track the (possibly advanced)
    // playhead so we present the landed frame, not the pre-drop one.
    const int64_t want_now = current_frame_.load() + 1;
    if (want_now != want) {
        want = want_now;
        reset_ready();
    }

    // Pop an already-decoded frame from the lookahead buffer. If it was
    // somehow not prefetched (fast seek right at the boundary), fall back to
    // an inline decode so we never skip a frame.
    canvas::core::RenderFramePtr frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_.empty() && ready_base_ == want) {
            frame = std::move(ready_.front());
            ready_.pop_front();
            ++ready_base_;
        }
    }
    if (!frame) {
        fill_lookahead(want);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_.empty()) {
            frame = std::move(ready_.front());
            ready_.pop_front();
            ++ready_base_;
        }
    }

    const double rate = media_rate_at(want);
    const auto interval = rate > 0.0
        ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate))
        : Clock::duration{33333};
    const auto now = Clock::now();
    if (next_present_ + interval * 4 < now) next_present_ = now;

    static auto last_present_log = Clock::now();
    static int cadence_log_ = 0;
    if (playback_debug() && (cadence_log_++ % 30) == 0) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_present_log).count();
        last_present_log = now;
        qDebug() << "playback: cadence rate=" << rate << "interval_ms="
                 << std::chrono::duration<double, std::milli>(interval).count()
                 << "measured_30f_ms=" << ms;
    }

    current_frame_.store(want);
    static auto last_exact = Clock::now();
    static int exact_log_ = 0;
    if (playback_debug() && (exact_log_++ % 10) == 0) {
        const double d = std::chrono::duration<double, std::milli>(now - last_exact).count();
        last_exact = now;
        qDebug() << "playback: exact_present_delta_ms=" << d << "frame=" << want
                 << "target_interval_ms="
                 << std::chrono::duration<double, std::milli>(interval).count();
    }
    static int present_log_ = 0;
    if (playback_debug() && (present_log_++ % 30) == 0)
        qDebug() << "playback: present frame" << want << "ready=" << (frame != nullptr)
                 << "audio_active=" << audio_.is_active();

    // ALWAYS-ON playback health (~1/s via qWarning so default runs capture it).
    // `cadence_ms` is the true present-to-present interval; above `target_ms`,
    // video is stalling behind realtime (what makes audio run ahead / artifact).
    // `fps_window` counts actual contiguous frame-walks in the last second (seeks
    // and drop-to-realtime jumps are excluded), so a value far below the rate
    // implies bursty present that is also audible as chopped audio.
    static auto last_health_log = Clock::now();
    static int health_log_ = 0;
    static int64_t last_health_frame = 0;   // updated EVERY present (for cadence/fps)
    static double cadence_ms = 0.0;
    static bool first_cadence = true;
    const double ms_since_present =
        std::chrono::duration<double, std::milli>(now - last_present_ts_).count();
    last_present_ts_ = now;
    cadence_ms = first_cadence ? 0.0 : ms_since_present;
    first_cadence = false;
    // Track the walked frame on every present so contiguity is judged against the
    // immediately preceding frame (not last second's sample, which made it ~60
    // frames stale and thus always "(non-contiguous)").
    bool contiguous_this = false;
    if (!first_cadence || last_health_frame != 0)
        contiguous_this = want == last_health_frame + 1;
    last_health_frame = want;
    contig_delta_ += contiguous_this ? 1 : 0;   // contiguous walks since last log
    if (++health_log_ == 1 || now - last_health_log >= std::chrono::seconds(1)) {
        // Only count contiguous same-rate presents toward fps_window; a seek or
        // drop-to-realtime jump carries the playhead forward without walking frames,
        // which would otherwise inflate (or distort) the presented-frames-per-sec.
        const double win_s = std::chrono::duration<double>(now - last_health_log).count();
        last_health_log = now;
        const int64_t walk = contig_delta_;
        const bool contiguous = walk > 0;
        contig_delta_ = 0;
        const bool has_pix = frame && (frame->a || frame->nv12);
        const double interval_ms = std::chrono::duration<double, std::milli>(interval).count();
        int maxedge = 0, nv12w = 0;
        if (frame) {
            if (frame->a) maxedge = std::max(frame->a->width, frame->a->height);
            if (frame->nv12) nv12w = frame->nv12->width;
        }
        qWarning().nospace()
            << "[play] frame=" << want
            << " hit=" << (has_pix ? "OK" : "NONE")
            << " maxedge=" << maxedge << " nv12w=" << nv12w
            << " cadence_ms=" << QString::number(cadence_ms, 'f', 0)
            << " target_ms=" << QString::number(interval_ms, 'f', 0)
            << " fps_window="
            << QString::number(contiguous && win_s > 0.0 ? walk / win_s : 0.0, 'f', 1)
            << (contiguous ? "" : " (non-contiguous)")
            << " audio=" << audio_.is_active();
    }

    emit frame_ready(std::move(frame));
    emit position_changed(want);

    if (audio_.is_active())
        audio_.play_step(want, std::chrono::duration<double>(interval).count(),
                         sonicsync_.seek_hold_active());

    next_present_ += interval;
}

canvas::core::RenderFramePtr SequenceController::frame_for_playhead(int64_t seq_frame) {
    auto out = std::make_shared<canvas::core::RenderFrame>();
    if (!project_) return out;
    return decoder_.frame(*project_, seq_frame);
}

// Low-resolution variant used only for scrubbing. Bypasses the full-res cache on
// the READ side but crucially does NOT put the reduced frame back into it, so a
// preview does not displace (or get returned as) a full-res playback frame.
canvas::core::RenderFramePtr SequenceController::frame_for_playhead_preview(int64_t seq_frame,
                                                                        int max_dim) {
    auto out = std::make_shared<canvas::core::RenderFrame>();
    if (!project_) return out;
    return decoder_.preview(*project_, seq_frame, max_dim);
}


}
