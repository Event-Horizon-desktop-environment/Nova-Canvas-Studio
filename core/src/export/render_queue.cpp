#include "canvas/core/export/render_queue.hpp"

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/util/log.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <thread>

namespace canvas::core {

namespace {

// Wall-clock stamp for finished jobs: "HH:MM:SS" (local time) so the queue's
// finished cards read like Resolve's ("Finished 14:22:03").
std::string wall_clock_hhmmss() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[16]{};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return std::string(buf);
}

}  // namespace

RenderJobSnapshot render_job_snapshot(const RenderJob& job) {
    RenderJobSnapshot s;
    s.id = job.id;
    s.name = job.name;
    s.output_path = job.output_path;
    s.settings = job.settings;
    s.total_frames = job.total_frames;
    s.status = static_cast<int>(job.status);
    s.progress = job.progress;
    s.render_fps = job.render_fps;
    s.error = job.error;
    s.elapsed_seconds = job.elapsed_seconds;
    s.frames_rendered = job.frames_rendered;
    s.finished_at = job.finished_at;
    return s;
}

RenderJob render_job_from_snapshot(const RenderJobSnapshot& snap) {
    RenderJob j;
    j.id = snap.id;
    j.name = snap.name;
    j.output_path = snap.output_path;
    j.settings = snap.settings;
    j.total_frames = snap.total_frames;
    j.status = static_cast<RenderJob::Status>(snap.status);
    // A job persisted while it was actively rendering cannot resume; stage it
    // so the user can re-run it (or drop it) with the rest of the queue.
    if (j.status == RenderJob::Status::Rendering) j.status = RenderJob::Status::Queued;
    j.progress = snap.progress;
    j.render_fps = snap.render_fps;
    j.error = snap.error;
    j.elapsed_seconds = snap.elapsed_seconds;
    j.frames_rendered = snap.frames_rendered;
    j.finished_at = snap.finished_at;
    return j;
}

RenderQueue::RenderQueue() {
    running_ = true;
    worker_ = std::thread(&RenderQueue::worker, this);
}

RenderQueue::~RenderQueue() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void RenderQueue::enqueue(RenderJob job) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        job.id = next_id_++;
        job.status = RenderJob::Status::Queued;
        jobs_.push_back(std::move(job));
        enqueued_at_[job.id] = std::chrono::steady_clock::now();
    }
    cv_.notify_one();
    if (on_changed) on_changed();
}

void RenderQueue::enqueue_individual(
    const DeliverSettings& base,
    const std::function<bool(int index, ExportSettings& out)>& per_clip) {
    int index = 0;
    while (true) {
        ExportSettings es;
        if (!per_clip(index, es)) break;
        DeliverSettings local = base;
        RenderJob job;
        job.settings = std::move(local);
        job.name = "Clip " + std::to_string(index + 1);
        es.video_codec = video_encoder_name(video_codec_from_string(base.video.codec),
                                            EncoderBackend::Auto, es.format, nullptr);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            job.id = next_id_++;
            job.status = RenderJob::Status::Queued;
            jobs_.push_back(std::move(job));
            enqueued_at_[job.id] = std::chrono::steady_clock::now();
        }
        ++index;
    }
    cv_.notify_one();
    if (on_changed) on_changed();
}

void RenderQueue::start() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        start_requested_ = true;
    }
    cv_.notify_all();
    if (on_changed) on_changed();
}

std::size_t RenderQueue::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return jobs_.size();
}

void RenderQueue::clear_finished() {
    std::lock_guard<std::mutex> lk(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const RenderJob& j) {
                                   return j.status == RenderJob::Status::Completed ||
                                          j.status == RenderJob::Status::Failed ||
                                          j.status == RenderJob::Status::Cancelled;
                               }),
                jobs_.end());
    if (on_changed) on_changed();
}

void RenderQueue::clear_queued() {
    std::lock_guard<std::mutex> lk(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const RenderJob& j) {
                                   return j.status != RenderJob::Status::Rendering;
                               }),
                jobs_.end());
    if (on_changed) on_changed();
}

void RenderQueue::cancel(uint64_t id) {
    double prog = 0.0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& j : jobs_)
            if (j.id == id) {
                j.status = RenderJob::Status::Cancelled;
                prog = j.progress;
            }
    }
    log::log_warning("[render:q] CANCEL id=%llu progress=%.0f%%", (unsigned long long)id,
                     prog * 100.0);
    if (on_changed) on_changed();
}

void RenderQueue::remove(uint64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->id != id) continue;
        if (it->status == RenderJob::Status::Rendering)
            it->status = RenderJob::Status::Cancelled;
        else
            jobs_.erase(it);
        break;
    }
    if (on_changed) on_changed();
}

void RenderQueue::cancel_all() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& j : jobs_)
        if (j.status == RenderJob::Status::Queued)
            j.status = RenderJob::Status::Cancelled;
    if (on_changed) on_changed();
}

void RenderQueue::clear_all() {
    std::lock_guard<std::mutex> lk(mutex_);
    jobs_.clear();
    if (on_changed) on_changed();
}

void RenderQueue::restore(const std::vector<RenderJob>& jobs) {
    std::lock_guard<std::mutex> lk(mutex_);
    jobs_ = jobs;
    uint64_t max_id = 0;
    for (const auto& j : jobs_) max_id = std::max(max_id, j.id);
    next_id_.store(max_id + 1);
    start_requested_ = false;
    if (on_changed) on_changed();
}

double RenderQueue::queue_progress() const {
    std::lock_guard<std::mutex> lk(mutex_);
    double total = 0, done = 0;
    for (const auto& j : jobs_) {
        total += 1.0;
        if (j.status == RenderJob::Status::Completed) done += 1.0;
        else if (j.status == RenderJob::Status::Rendering) done += j.progress;
        else if (j.status == RenderJob::Status::Cancelled || j.status == RenderJob::Status::Failed)
            done += 1.0;  // treat as settled
    }
    return total > 0 ? done / total : 0.0;
}

bool RenderQueue::is_busy() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& j : jobs_)
        if (j.status == RenderJob::Status::Queued || j.status == RenderJob::Status::Rendering)
            return true;
    return false;
}

std::vector<RenderJob> RenderQueue::jobs() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return jobs_;
}

void RenderQueue::worker() {
    using Project = canvas::core::Project;
    // Idle->busy->idle edge tracker for the always-on [render:q] lines. `idle`
    // means the worker just found nothing to drain (pre-start or between batches).
    static bool s_was_busy = false;
    static uint64_t s_last_dispatch = 0;
    while (true) {
        RenderJob local;
        uint64_t id = 0;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] {
                if (stop_) return true;
                if (!start_requested_) return false;
                // A batch is being drained; as soon as every queued job has been
                // picked up, clear the start flag so the queue waits again.
                bool any_queued = false;
                for (auto& j : jobs_)
                    if (j.status == RenderJob::Status::Queued) { any_queued = true; break; }
                if (!any_queued) { start_requested_ = false; return false; }
                return true;
            });
            if (stop_) return;
            for (auto& j : jobs_) {
                if (j.status != RenderJob::Status::Queued) continue;
                j.status = RenderJob::Status::Rendering;
                local = j;  // copy to a local so we stay valid across enqueue/clear
                id = j.id;
                break;
            }
        }
        if (id == 0) {
            // Worker found nothing to pick up: idle tick. Log the edge ONCE per
            // idle session (not every cv wake), so idle->busy->idle churn around
            // a batch shows up without spamming.
            if (s_was_busy) {
                s_was_busy = false;
                log::log_warning("[render:q] worker IDLE after job %llu",
                                 (unsigned long long)s_last_dispatch);
            }
            continue;
        }
        s_last_dispatch = id;
        if (!s_was_busy) {
            s_was_busy = true;
            log::log_warning("[render:q] worker BUSY dispatch id=%llu", (unsigned long long)id);
        }

        // Always-on dispatch line: time this job sat in the queue (user staged it
        // then hit start), how many others were queued behind it, and whether the
        // worker was coming from idle. A backlog here with a busy renderer means
        // the user queued more frames than the machine can chew through.
        double wait_ms = 0.0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const auto it = enqueued_at_.find(id);
            if (it != enqueued_at_.end()) {
                wait_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - it->second).count();
                enqueued_at_.erase(it);
            }
        }
        std::size_t depth = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const auto& j : jobs_)
                if (j.status == RenderJob::Status::Queued) ++depth;
        }
        log::log_warning("[render:q] dispatch id=%llu '%s' depth_behind=%zu wait_ms=%.0f",
                    (unsigned long long)id, local.name.c_str(), depth, wait_ms);

        if (on_job_started) on_job_started(id);
        CANVAS_LOG("render queue: job %llu '%s' START out='%s' frames=%lld",
               (unsigned long long)id, local.name.c_str(), local.output_path.c_str(),
               (long long)local.total_frames);

        std::shared_ptr<const Project> project;
        decltype(project_resolver_) resolver;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            project = active_project_;
            resolver = project_resolver_;
        }

        auto started = std::chrono::steady_clock::now();
        const int64_t total = local.total_frames;

        ExportControl ctrl;
        std::atomic<bool> cancelled{false};
        std::atomic<double> fps{0.0};
        ctrl.should_cancel = [&] { return cancelled.load(); };
        ctrl.on_progress = [&](double p, const std::string& phase) {
            (void)phase;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - started).count();
            double f = p * total;
            if (secs > 0) fps.store(f / secs);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& j : jobs_) {
                    if (j.id != id) continue;
                    j.progress = p;
                    j.render_fps = fps.load();
                    j.frames_rendered = (int64_t)std::llround(f);
                    j.elapsed_seconds = secs;
                }
            }
            if (on_changed) on_changed();
        };

        std::string error;
        bool ok = false;
        if (!cancelled.load()) {
            ExportSettings es = to_export_settings(local.settings);
            es.output_path = local.output_path;
            es.duration_frames = local.total_frames;
            ok = run_job(project, es, resolver, &ctrl, &cancelled, &error);
        } else {
            error = "Cancelled before start.";
        }

        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - started).count();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto& j : jobs_) {
                if (j.id != id) continue;
                j.elapsed_seconds = secs;
                if (cancelled.load() || error.find("Cancelled") != std::string::npos)
                    j.status = RenderJob::Status::Cancelled;
                else if (ok) {
                    j.status = RenderJob::Status::Completed;
                    j.progress = 1.0;
                    j.finished_at = wall_clock_hhmmss();
                } else {
                    j.status = RenderJob::Status::Failed;
                    j.error = error;
                }
            }
        }
        if (on_job_finished) on_job_finished(id);
        if (on_changed) on_changed();
        if (ok)
            CANVAS_LOG("render queue: job %llu '%s' DONE (frames=%lld, %.1fs)",
                   (unsigned long long)id, local.name.c_str(), (long long)local.total_frames,
                   secs);
        else
            ::canvas::core::log::log_error("render queue: job %llu '%s' FAILED: %s",
                                       (unsigned long long)id, local.name.c_str(),
                                       error.c_str());
    }
}

void RenderQueue::set_active_project(
    std::shared_ptr<const canvas::core::Project> project,
    std::function<bool(const ExportSettings&, std::shared_ptr<const canvas::core::Project>&)>
        project_resolver) {
    std::lock_guard<std::mutex> lk(mutex_);
    active_project_ = std::move(project);
    project_resolver_ = std::move(project_resolver);
}

bool RenderQueue::run_job(
    const std::shared_ptr<const canvas::core::Project>& project, const ExportSettings& es,
    const std::function<bool(const ExportSettings&, std::shared_ptr<const canvas::core::Project>&)>&
        resolver,
    ExportControl* ctrl, std::atomic<bool>* cancelled, std::string* error) {
    if (resolver) {
        std::shared_ptr<const canvas::core::Project> resolved;
        if (resolver(es, resolved) && resolved)
            return export_project(*resolved, es, ctrl, error);
    }
    if (!project) {
        if (error) *error = "No project set for render job.";
        return false;
    }
    return export_project(*project, es, ctrl, error);
}

}  // namespace canvas::core
