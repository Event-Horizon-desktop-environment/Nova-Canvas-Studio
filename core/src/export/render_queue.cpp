#include "canvas/core/export/render_queue.hpp"

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/project/project.hpp"
#include "canvas/core/util/log.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace canvas::core {

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

void RenderQueue::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& j : jobs_)
        if (j.id == id)
            j.status = RenderJob::Status::Cancelled;
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
        if (id == 0) continue;

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
