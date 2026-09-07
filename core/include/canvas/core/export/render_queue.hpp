#pragma once

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/project/project.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace canvas::core {

// One render job in the queue.
struct RenderJob {
    uint64_t id = 0;
    std::string name;            // display name / file name
    DeliverSettings settings;
    std::string output_path;
    int64_t total_frames = 0;

    enum class Status { Queued, Rendering, Completed, Failed, Cancelled };
    Status status = Status::Queued;
    double progress = 0.0;       // 0..1
    double render_fps = 0.0;     // measured frame rate of the active render
    std::string error;
    double elapsed_seconds = 0.0;
    int64_t frames_rendered = 0;
    std::string finished_at;     // wall clock "HH:MM:SS" stamped on Completion
};

// Plain-data mirrors used to persist jobs inside the Project (the PROJECTS
// ship with their render queue). No thread state is carried across.
RenderJobSnapshot render_job_snapshot(const RenderJob& job);
// Rebuilds a job from its snapshot; a job persisted mid-render comes back
// Queued so the user re-runs (or clears) it rather than resuming.
RenderJob render_job_from_snapshot(const RenderJobSnapshot& snap);

// A background render queue. `start()` runs a worker thread that drains queued
// jobs (calling export_project with progress/cancel callbacks). Thread-safe.
class RenderQueue {
public:
    RenderQueue();
    ~RenderQueue();
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    void enqueue(RenderJob job);
    // Add one job per timeline clip (IndividualClips scope); each gets its own
    // clip-range ExportSettings via `per_clip`.
    void enqueue_individual(const DeliverSettings& base,
                            const std::function<bool(int index, ExportSettings& out)>& per_clip);
    // Begin draining all currently-Queued jobs. Jobs do NOT start until start()
    // is called ("Add to Render Queue" only stages work); after the batch drains
    // the queue waits for the next start().
    void start();
    // Provide the active project + an optional per-job resolver; `project` is the
    // default fallback.
    void set_active_project(std::shared_ptr<const canvas::core::Project> project,
                            std::function<bool(const ExportSettings&,
                                               std::shared_ptr<const canvas::core::Project>&)>
                                project_resolver = {});
    std::size_t size() const;
    void clear_finished();
    // Removes every job that is not actively Rendering (queued, completed,
    // failed and cancelled all go), keeping only the in-flight export so the
    // panel can wipe a staged-but-unrendered queue mid-run.
    void clear_queued();
    void cancel(uint64_t id);
    // Removes a specific queued/finished job; a rendering job is cancelled
    // instead (the worker owns it until it settles).
    void remove(uint64_t id);
    void cancel_all();
    void clear_all();
    // Replaces the queue wholesale (used on project open to reinstate the jobs
    // saved with the project). Rendering statuses are demoted to Queued.
    void restore(const std::vector<RenderJob>& jobs);
    // Resolves the (weighted) queue progress 0..1.
    double queue_progress() const;
    bool is_busy() const;

    // Snapshot copy of all jobs (thread-safe).
    std::vector<RenderJob> jobs() const;

    // Raised by the worker on any change; connect to refresh the GUI (wrapped in
    // queued connections upstream).
    std::function<void()> on_changed;
    std::function<void(uint64_t)> on_job_started;
    std::function<void(uint64_t)> on_job_finished;

private:
    void worker();
    static bool run_job(const std::shared_ptr<const canvas::core::Project>& project,
                        const ExportSettings& es,
                        const std::function<bool(const ExportSettings&,
                                                 std::shared_ptr<const canvas::core::Project>&)>& resolver,
                        ExportControl* ctrl, std::atomic<bool>* cancelled, std::string* error);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RenderJob> jobs_;
    std::atomic<uint64_t> next_id_{1};
    std::thread worker_;
    bool running_ = false;
    bool stop_ = false;
    bool start_requested_ = false;  // worker drains queued jobs only while set
    std::shared_ptr<const canvas::core::Project> active_project_;
    std::function<bool(const ExportSettings&, std::shared_ptr<const canvas::core::Project>&)>
        project_resolver_;
};

}  // namespace canvas::core
