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
};

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
    // clip-range ExportSettings via `per_clip` mapper.
    void enqueue_individual(const DeliverSettings& base,
                            const std::function<bool(int index, ExportSettings& out)>& per_clip);
    // Begin draining all currently-Queued jobs. Enqueued jobs do NOT start until
    // start() is called (so "Add to Render Queue" only stages work and the user
    // explicitly triggers rendering). After the batch is drained the queue goes
    // back to waiting for a fresh start().
    void start();
    // Provide the active project + an optional resolver so jobs render the right
    // timeline (e.g. one job per clip). `project` is the default fallback.
    void set_active_project(std::shared_ptr<const canvas::core::Project> project,
                            std::function<bool(const ExportSettings&,
                                               std::shared_ptr<const canvas::core::Project>&)>
                                project_resolver = {});
    std::size_t size() const;
    void clear_finished();
    void cancel(uint64_t id);
    void cancel_all();
    void clear_all();
    // Resolves the (weighted) queue progress 0..1.
    double queue_progress() const;
    bool is_busy() const;

    // Snapshot copy of all jobs (thread-safe).
    std::vector<RenderJob> jobs() const;

    // Raised by the worker thread on any change; connect these to refresh the
    // GUI. (Qt code wraps these in queued connections.)
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
