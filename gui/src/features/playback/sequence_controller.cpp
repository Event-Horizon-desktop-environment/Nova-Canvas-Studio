#include "sequence_controller.hpp"
#include "Logging.hpp"

#include "canvas/core/gpu/cuda_convert.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace canvas::gui {

using Clock = std::chrono::steady_clock;

static bool media_set_identical(const canvas::core::Project& a, const canvas::core::Project& b) {
    const auto& ma = a.media;
    const auto& mb = b.media;
    if (ma.size() != mb.size()) return false;
    for (std::size_t i = 0; i < ma.size(); ++i) {
        const auto& x = ma[i];
        const auto& y = mb[i];
        if (x.id != y.id || x.path != y.path || x.fps != y.fps ||
            x.width != y.width || x.height != y.height ||
            x.total_frames != y.total_frames || x.has_audio != y.has_audio) {
            return false;
        }
    }
    return true;
}

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
        const auto is_seek_cmd = [](Command c) {
            return c == Command::Seek || c == Command::SeekPreview;
        };
        queue_.erase(
            std::remove_if(queue_.begin(), queue_.end(),
                           [&](const Request& r) {
                               return (is_seek_cmd(request.command) && is_seek_cmd(r.command)) ||
                                      (request.command == Command::Pause && r.command == Command::Play) ||
                                      (request.command == Command::Play && is_seek_cmd(r.command)) ||
                                      (request.command == Command::SwapProject &&
                                       r.command == Command::SwapProject);
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

void SequenceController::swap_project(std::shared_ptr<const canvas::core::Project> project) {
    if (!project) return;
    push({Command::SwapProject, 0, std::move(project), {}});
}

void SequenceController::update_audio_mix(std::shared_ptr<const canvas::core::Project> project) {
    push({Command::UpdateAudioMix, 0, std::move(project), {}});
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
void SequenceController::release_audio() {
    push({Command::ReleaseAudio, 0, {}, {}});
}
void SequenceController::seek(const int64_t frame_number) {
    push({Command::Seek, frame_number, {}, {}});
}
void SequenceController::seek_preview(const int64_t frame_number) {
    push({Command::SeekPreview, frame_number, {}, {}});
}

void SequenceController::begin_scrub() {
    scrubbing_.store(true);
    if (!scrub_drag_active_) {
        scrub_drag_active_ = true;
        scrub_start_ = Clock::now();
        scrub_first_ = current_frame_.load();
        scrub_previews_ = 0;
        scrub_preview_hits_ = scrub_preview_misses_ = 0;
        scrub_preview_evictions_ = 0;
        scrub_preview_ms_sum_ = 0.0;
        scrub_preview_ms_max_ = 0.0;
        audio_.begin_scrub();
        qDebug() << "[scrub] BEGIN playing=" << playing_.load()
                   << "enabled=" << scrub_audio_enabled_.load() << "frame=" << current_frame_.load();
    }
}
void SequenceController::end_scrub() {
    scrubbing_.store(false);
    const double drag_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - scrub_start_).count();
    const auto pv = decoder_.take_preview_stats();
    scrub_preview_hits_ += pv.hits;
    scrub_preview_misses_ += pv.misses;
    scrub_preview_evictions_ += pv.evictions;
    const double avg_pv_ms =
        scrub_previews_ > 0 ? scrub_preview_ms_sum_ / static_cast<double>(scrub_previews_) : 0.0;
    qDebug().nospace()
        << "[scrub] END released_at=" << current_frame_.load()
        << " was_playing=" << playing_.load()
        << " audio_open=" << scrub_audio_open_
        << " drag_ms=" << QString::number(drag_ms, 'f', 0)
        << " first=" << scrub_first_
        << " span=" << (current_frame_.load() - scrub_first_)
        << " previews=" << scrub_previews_
        << " pv_hits=" << scrub_preview_hits_
        << " pv_misses=" << scrub_preview_misses_
        << " pv_evicts=" << scrub_preview_evictions_
        << " pv_avg_ms=" << QString::number(avg_pv_ms, 'f', 1)
        << " pv_max_ms=" << QString::number(scrub_preview_ms_max_, 'f', 1)
        << " repositions=" << audio_.repositions_since_begin()
        << " pending=" << audio_out_.pending_frames();
    scrub_drag_active_ = false;
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
            case Command::SwapProject:
                handle_swap_project(std::move(req.project));
                break;
            case Command::UpdateAudioMix:
                handle_update_audio_mix(std::move(req.project));
                break;
            case Command::AddMedia:
                handle_add_media(req.media);
                break;
            case Command::Play:
                handle_play();
                break;
            case Command::Pause:
                qDebug() << "[transport] PAUSE at=" << current_frame_.load();
                playing_.store(false);
                play_pause_intent_.store(false);
                if (audio_.is_active()) {
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
                qDebug() << "[transport] STEP delta=" << req.arg
                           << "-> target=" << (current_frame_.load() + req.arg);
                handle_seek(current_frame_.load() + req.arg);
                break;
            case Command::ReleaseAudio:
                playing_.store(false);
                play_pause_intent_.store(false);
                if (audio_.is_active()) {
                    audio_out_.set_hold_active(false);
                    audio_.close_output();
                }
                emit playback_changed(false);
                break;
            }
            continue;
        }

        if (playing_.load()) {
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
            {
                const double fill_ms =
                    std::chrono::duration<double, std::milli>(t_filled - t_loop).count();
                const double wait_ms =
                    std::chrono::duration<double, std::milli>(t_waited - t_before_wait).count();
                const double pres_ms =
                    std::chrono::duration<double, std::milli>(t_done - t_waited).count();
                const double late_ms =
                    std::chrono::duration<double, std::milli>(t_before_wait - nwp).count();
                static auto loop_agg_at = Clock::now();
                static int loop_agg_n = 0;
                static double loop_fill = 0.0, loop_wait = 0.0, loop_pres = 0.0;
                static double loop_late_max = 0.0;
                ++loop_agg_n;
                loop_fill += fill_ms;
                loop_wait += wait_ms;
                loop_pres += pres_ms;
                loop_late_max = std::max(loop_late_max, late_ms);
                const auto lnow = Clock::now();
                if (loop_agg_n == 1 || lnow - loop_agg_at >= std::chrono::seconds(1)) {
                    loop_agg_at = lnow;
                    qDebug().nospace()
                        << "[loop] n=" << loop_agg_n
                        << " fill_ms=" << QString::number(loop_fill / loop_agg_n, 'f', 2)
                        << " wait_ms=" << QString::number(loop_wait / loop_agg_n, 'f', 2)
                        << " present_ms=" << QString::number(loop_pres / loop_agg_n, 'f', 2)
                        << " late_max_ms=" << QString::number(loop_late_max, 'f', 2)
                        << " wake_by_cmd=" << wat
                        << " skipped=" << skipped;
                    loop_agg_n = 0;
                    loop_fill = loop_wait = loop_pres = 0.0;
                    loop_late_max = 0.0;
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
    static unsigned set_proj_n_ = 0;
    static auto set_proj_t0_ = Clock::now();
    const double since_ms = std::chrono::duration<double, std::milli>(
                                Clock::now() - set_proj_t0_)
                                .count();
    set_proj_t0_ = Clock::now();
    ++set_proj_n_;
    if (debug_enabled())
        qDebug().nospace()
            << "[playback] SET-PROJECT #" << set_proj_n_
            << " since_last_ms=" << QString::number(since_ms, 'f', 0)
            << " initial_frame=" << initial_frame
            << " playing=" << playing_.load();
    playing_.store(false);
    play_pause_intent_.store(false);
    emit playback_changed(false);

    if (project_ && project && media_set_identical(*project_, *project)) {
        audio_.update_project(project.get());
        project_ = std::move(project);
        fps_.store(project_->sequence.fps);
        total_frames_.store(project_->sequence.duration_frames());
        const int64_t cur = current_frame_.load();
        const int64_t anchor = initial_frame >= 0 ? initial_frame : (cur >= 0 ? cur : 0);
        handle_seek(anchor);
        return;
    }

    audio_.reset();
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
    const int64_t cur = current_frame_.load();
    const int64_t anchor = initial_frame >= 0 ? initial_frame : (cur >= 0 ? cur : 0);
    qDebug().nospace()
        << "[transport] SET-PROJECT anchor=" << anchor
        << " frames=" << total_frames_.load()
        << " fps=" << fps_.load()
        << " video_tracks=" << project_->sequence.video_tracks.size()
        << " audio_tracks=" << project_->sequence.audio_tracks.size()
        << " media=" << project_->media.size();
    handle_seek(anchor);
}

void SequenceController::handle_update_audio_mix(
    std::shared_ptr<const canvas::core::Project> project) {
    if (!project) return;
    audio_.update_project(project.get());
    project_ = std::move(project);
    if (debug_enabled())
        qDebug() << "playback: LIVE audio mix swap (playing=" << playing_.load() << ")";
}

void SequenceController::handle_swap_project(std::shared_ptr<const canvas::core::Project> project) {
    if (!project) return;
    audio_.update_project(project.get());
    project_ = std::move(project);
    fps_.store(project_->sequence.fps);
    total_frames_.store(project_->sequence.duration_frames());
    const int64_t cur = current_frame_.load();
    if (debug_enabled())
        qDebug() << "playback: GRADE swap (playing=" << playing_.load()
                 << ") re-present at frame=" << cur;
    if (cur >= 0) handle_seek(cur);
}

void SequenceController::handle_add_media(const canvas::core::MediaEntry& entry) {
    decoder_.add_media(entry);
    audio_.add_media(entry);
}

void SequenceController::handle_play() {
    if (!project_ || total_frames_.load() <= 0) return;
    play_pause_intent_.store(true);
    audio_out_.log_pipeline_stats("play-pre");
    if (current_frame_.load() >= total_frames_.load() - 1) handle_seek(0);
    reset_ready();
    audio_.open_output();
    audio_.rewind(current_frame_.load(), false);
    audio_.preroll(current_frame_.load(), kAudioLeadMs, playing_.load());
    if (audio_.is_active()) audio_out_.set_hold_active(true);
    playing_.store(true);
    next_present_ = Clock::now();
    play_t0_ = Clock::now();
    play_armed_ = true;
    aud_baseline_frames_ = audio_out_.audible_position_frames();
    aud_armed_ = audio_.is_active();
    audio_out_.log_pipeline_stats("play-post");
    qDebug() << "[transport] PLAY at=" << current_frame_.load()
               << "/" << total_frames_.load();
    emit playback_changed(true);
}

void SequenceController::handle_seek(const int64_t frame_number) {
    if (!project_) return;
    int64_t target = frame_number;
    const int64_t last = total_frames_.load();
    if (last > 0) target = std::clamp(target, int64_t{0}, last - 1);
    if (target < 0) target = 0;

    seek_arm_t0_ = Clock::now();
    seek_present_armed_ = true;
    seek_present_target_ = target;
    reset_ready();
    const bool was_playing = playing_.load();
    sonicsync_.begin_seek_hold(target);
    current_frame_.store(target);
    next_present_ = Clock::now();
    qDebug() << "[scrub] COMMIT seek_to=" << target
               << "playing=" << was_playing
               << "scrubbing=" << scrubbing_.load();
    const auto commit_t0 = Clock::now();
    audio_.rewind(target, playing_.load());
    const double rewind_ms = std::chrono::duration<double, std::milli>(Clock::now() - commit_t0).count();
    const auto dec_t0 = Clock::now();
    auto frame = frame_for_playhead(target);
    const double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - dec_t0).count();
    if (debug_enabled())
        qDebug() << "playback: SEEK to frame" << target << "got_frame=" << (frame != nullptr);
    sonicsync_.end_seek_hold();
    emit frame_ready(std::move(frame));
    emit position_changed(target);
    if (!was_playing && seek_present_armed_) {
        seek_present_armed_ = false;
        const double d =
            std::chrono::duration<double, std::milli>(Clock::now() - seek_arm_t0_).count();
        qDebug().nospace() << "[transport] seek->first_present at=" << target
                             << " latency_ms=" << QString::number(d, 'f', 0)
                             << " decode_ms=" << QString::number(decode_ms, 'f', 1);
    }
    const auto preroll_t0 = Clock::now();
    if (was_playing) audio_.preroll(target, kAudioLeadMs, was_playing);
    const double preroll_ms = std::chrono::duration<double, std::milli>(Clock::now() - preroll_t0).count();
    const auto warm_t0 = Clock::now();
    if (was_playing) warm_lookahead(target + 1);
    const double warm_ms = std::chrono::duration<double, std::milli>(Clock::now() - warm_t0).count();
    qDebug() << "[scrub] COMMIT-times rewind_ms=" << rewind_ms
               << "preroll_ms=" << preroll_ms
               << "decode_ms=" << decode_ms
               << "warm_ms=" << warm_ms
               << "total_ms=" << (rewind_ms + preroll_ms + decode_ms + warm_ms);
}

void SequenceController::handle_seek_preview(const int64_t frame_number) {
    if (!project_) return;
    if (playing_.load() && !scrubbing_.load()) {
        handle_seek(frame_number);
        return;
    }
    int64_t target = frame_number;
    const int64_t last = total_frames_.load();
    if (last > 0) target = std::clamp(target, int64_t{0}, last - 1);
    if (target < 0) target = 0;
    current_frame_.store(target);

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
    if (scrub_drag_active_) {
        ++scrub_previews_;
        scrub_preview_ms_sum_ += preview_ms;
        scrub_preview_ms_max_ = std::max(scrub_preview_ms_max_, preview_ms);
    }
    const bool got = frame != nullptr && (frame->nv12 || frame->a || frame->b);
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
    const int diag_nv12w = frame && frame->nv12 ? frame->nv12->width : 0;
    const int diag_aw = frame && frame->a ? std::max(frame->a->width, frame->a->height) : 0;
    const int diag_bw = frame && frame->b ? std::max(frame->b->width, frame->b->height) : 0;
    const bool diag_has_pix = frame && (frame->nv12 || frame->a || frame->b);
    emit frame_ready(std::move(frame));
    emit position_changed(target);

    qDebug().nospace()
        << "[scrub] target=" << target << " hit=" << (diag_has_pix ? "OK" : "BLACK")
        << " nv12=" << diag_nv12w
        << " a=" << diag_aw
        << " b=" << diag_bw
        << " decode_ms=" << QString::number(preview_ms, 'f', 1)
        << " playing=" << playing_.load();

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
    double scrub_audio_ms = 0.0;
    if (playing_.load() && audio_.is_active()) {
        const auto s0 = Clock::now();
        audio_.play_step(target, 1.0 / fps_.load(), sonicsync_.seek_hold_active());
        scrub_audio_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
    }

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

    if ((scrub_log_tick_ & 3u) == 0u || grain_ms > 0.0 || precache_ms > 0.0)
        qDebug() << "[scrub] PREVIEW-times decode_ms=" << preview_ms
                   << "grain_ms=" << grain_ms
                   << "scrub_audio_ms=" << scrub_audio_ms
                   << "precache_ms=" << precache_ms
                   << "last_target=" << last_scrub_target_;
}

void SequenceController::warm_lookahead(const int64_t start_frame) {
    if (!project_ || start_frame < 0) return;
    const int64_t last = total_frames_.load();
    if (last > 0 && start_frame >= last) return;
    const int64_t end = last > 0 ? std::min(start_frame + static_cast<int64_t>(kLookahead), last)
                                 : start_frame + static_cast<int64_t>(kLookahead);
    const double fps_v = fps_.load();
    const double budget_ms =
        fps_v > 0.0 ? 1100.0 / fps_v : 33.0;
    const auto t0 = Clock::now();
    for (int64_t f = start_frame; f < end; ++f) {
        if (!playing_.load()) return;
        frame_for_playhead(f);
        if (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() >= budget_ms)
            break;
    }
}

void SequenceController::present_next() {
    int64_t want = current_frame_.load() + 1;
    if (want >= total_frames_.load()) {
        playing_.store(false);
        play_pause_intent_.store(false);
        emit playback_changed(false);
        return;
    }

    const double rate_at_want = fps_.load();
    if (rate_at_want > 0.0) {
        const auto intv_us =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate_at_want));
        const auto lateness = next_present_ == Clock::time_point{}
                                  ? Clock::duration{}
                                  : Clock::now() - next_present_;
        const auto margin = std::chrono::duration_cast<Clock::duration>(intv_us + intv_us / 4);
        if (lateness > margin) {
            const auto hard_lateness =
                std::chrono::duration_cast<Clock::duration>(intv_us + intv_us / 4 +
                                                            std::chrono::milliseconds(1000));
            if (lateness <= hard_lateness) {
                next_present_ = Clock::now();
            } else {
            const int64_t owed = static_cast<int64_t>((lateness + intv_us / 2) / intv_us);
            int64_t target_catch = current_frame_.load() + owed;
            ++drop_events_;
            drop_frames_ += owed;

            int64_t aud_seq = audio_.audible_seq_frame(current_frame_.load());
            {
                const int64_t capped = sonicsync_.reconcile(
                    target_catch, aud_seq, static_cast<int64_t>(kLookahead),
                    total_frames_.load());
                if (capped != target_catch) {
                    if (playback_debug())
                        qDebug() << "playback: drop capped audible_seq=" << aud_seq
                                 << "target=" << target_catch << "->" << capped;
                    ++cap_events_;
                    target_catch = capped;
                }
            }

            const int64_t new_frame = std::min(target_catch, total_frames_.load() - 1);
            reset_ready();
            current_frame_.store(new_frame);
            audio_.advance_feed_for_drop(new_frame);
            next_present_ = Clock::now();
            }
        }
    }

    const int64_t want_now = current_frame_.load() + 1;
    if (want_now != want) {
        want = want_now;
        reset_ready();
    }

    canvas::core::RenderFramePtr frame;
    const bool popped_ready = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_.empty() && ready_base_ == want) {
            frame = std::move(ready_.front());
            ready_.pop_front();
            ++ready_base_;
        }
        return frame != nullptr;
    }();
    if (!frame) {
        fill_lookahead(want);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_.empty()) {
            frame = std::move(ready_.front());
            ready_.pop_front();
            ++ready_base_;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ready_depth_ = static_cast<int64_t>(ready_.size());
    }
    if (popped_ready) ++present_ready_hits_;
    else if (frame) ++present_inline_;

    const double rate = fps_.load();
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

    const double dt_present = play_armed_
        ? std::chrono::duration<double, std::milli>(Clock::now() - play_t0_).count()
        : 0.0;
    if (play_armed_) {
        play_armed_ = false;
        qDebug().nospace()
            << "[transport] play->first_present frame=" << want
            << " latency_ms=" << QString::number(dt_present, 'f', 0);
    }
    if (aud_armed_ && audio_.is_active() &&
        audio_out_.audible_position_frames() > aud_baseline_frames_) {
        aud_armed_ = false;
        const double d =
            std::chrono::duration<double, std::milli>(Clock::now() - play_t0_).count();
        qDebug().nospace()
            << "[transport] play->first_audible frame=" << want
            << " latency_ms=" << QString::number(d, 'f', 0);
    }
    if (seek_present_armed_ && want == seek_present_target_) {
        seek_present_armed_ = false;
        const double d =
            std::chrono::duration<double, std::milli>(Clock::now() - seek_arm_t0_).count();
        qDebug().nospace() << "[transport] seek->first_present at=" << want
                             << " latency_ms=" << QString::number(d, 'f', 0);
    }

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

    static auto last_health_log = Clock::now();
    static int health_log_ = 0;
    static int64_t last_health_frame = 0;
    static double cadence_ms = 0.0;
    static bool first_cadence = true;
    const double ms_since_present =
        std::chrono::duration<double, std::milli>(now - last_present_ts_).count();
    last_present_ts_ = now;
    cadence_ms = first_cadence ? 0.0 : ms_since_present;
    first_cadence = false;
    bool contiguous_this = false;
    if (!first_cadence || last_health_frame != 0)
        contiguous_this = want == last_health_frame + 1;
    last_health_frame = want;

    static auto last_slow_log = Clock::now();
    const double interval_ms_spike =
        std::chrono::duration<double, std::milli>(interval).count();
    if (contiguous_this && ms_since_present > interval_ms_spike * 2.5 &&
        now - last_slow_log >= std::chrono::milliseconds(3000)) {
        last_slow_log = now;
        const auto gs_slow = decoder_.take_grade_stats();
        const double gavg_slow =
            gs_slow.samples > 0 ? gs_slow.ms_sum / static_cast<double>(gs_slow.samples) : 0.0;
        qWarning().nospace()
            << "[play] SLOW-PRESENT frame=" << want
            << " delay_ms=" << QString::number(ms_since_present, 'f', 1)
            << " target_ms=" << QString::number(interval_ms_spike, 'f', 1)
            << " grade_avg_ms=" << QString::number(gavg_slow, 'f', 2)
            << " ready=" << last_ready_depth_;
    }

    contig_delta_ += contiguous_this ? 1 : 0;
    if (++health_log_ == 1 || now - last_health_log >= std::chrono::seconds(1)) {
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
        const auto gs = decoder_.take_grade_stats();
        const double gavg = gs.samples > 0 ? gs.ms_sum / static_cast<double>(gs.samples) : 0.0;
        qDebug().nospace()
            << "[play] frame=" << want
            << " hit=" << (has_pix ? "OK" : "NONE")
            << " maxedge=" << maxedge << " nv12w=" << nv12w
            << " cadence_ms=" << QString::number(cadence_ms, 'f', 0)
            << " target_ms=" << QString::number(interval_ms, 'f', 0)
            << " fps_window="
            << QString::number(contiguous && win_s > 0.0 ? walk / win_s : 0.0, 'f', 1)
            << (contiguous ? "" : " (non-contiguous)")
            << " ready=" << last_ready_depth_
            << " ready_hits=" << present_ready_hits_
            << " inline=" << present_inline_
            << " drops=" << drop_events_
            << " drop_frames=" << drop_frames_
            << " cap=" << cap_events_
            << " hold_max_ms=" << QString::number(sonicsync_.hold_stats().hold_ms_max, 'f', 0)
            << " hold_cnt=" << sonicsync_.hold_stats().hold_count
            << " grade_avg_ms=" << QString::number(gavg, 'f', 2)
            << " grade_n=" << gs.samples
            << " audio=" << audio_.is_active();
        present_ready_hits_ = present_inline_ = 0;
        drop_events_ = drop_frames_ = cap_events_ = 0;
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

canvas::core::RenderFramePtr SequenceController::frame_for_playhead_preview(int64_t seq_frame,
                                                                        int max_dim) {
    auto out = std::make_shared<canvas::core::RenderFrame>();
    if (!project_) return out;
    return decoder_.preview(*project_, seq_frame, max_dim);
}


}
