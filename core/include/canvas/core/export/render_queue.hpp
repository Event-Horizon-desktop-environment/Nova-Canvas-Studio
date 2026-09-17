#pragma once

#include "canvas/core/export/deliver_preset.hpp"
#include "canvas/core/media/frame.hpp"
#include "canvas/core/project/project.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace canvas::core {

struct RenderJob {
    uint64_t id = 0;
    std::string name;
    DeliverSettings settings;
    std::string output_path;
    int64_t total_frames = 0;
    int priority = 0;

    enum class Status { Queued, Rendering, Completed, Failed, Cancelled };
    Status status = Status::Queued;
    double progress = 0.0;
    double render_fps = 0.0;
    std::string error;
    double elapsed_seconds = 0.0;
    int64_t frames_rendered = 0;
    std::string finished_at;
};

RenderJobSnapshot render_job_snapshot(const RenderJob& job);
RenderJob render_job_from_snapshot(const RenderJobSnapshot& snap);

class RenderQueue {
public:
    RenderQueue();
    ~RenderQueue();
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    void enqueue(RenderJob job);
    void enqueue_individual(const DeliverSettings& base,
                            const std::function<bool(int index, ExportSettings& out)>& per_clip);
    void start();
    void set_active_project(std::shared_ptr<const canvas::core::Project> project,
                            std::function<bool(const ExportSettings&,
                                               std::shared_ptr<const canvas::core::Project>&)>
                                project_resolver = {});
    std::size_t size() const;
    void clear_finished();
    void clear_queued();
    void cancel(uint64_t id);
    void remove(uint64_t id);
    void cancel_all();
    void clear_all();
    void set_paused(bool paused);
    [[nodiscard]] bool is_paused() const;
    void set_priority(uint64_t id, int priority);
    void restore(const std::vector<RenderJob>& jobs);
    double queue_progress() const;
    bool is_busy() const;

    std::vector<RenderJob> jobs() const;

    std::function<void()> on_changed;
    std::function<void(uint64_t)> on_job_started;
    std::function<void(uint64_t)> on_job_finished;
    std::function<void(VideoFramePtr)> on_preview_frame;

private:
    void worker();
    static bool run_job(const std::shared_ptr<const canvas::core::Project>& project,
                        const ExportSettings& es,
                        const std::function<bool(const ExportSettings&,
                                                 std::shared_ptr<const canvas::core::Project>&)>& resolver,
                        ExportControl* ctrl, std::string* error);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RenderJob> jobs_;
    std::atomic<uint64_t> next_id_{1};
    std::thread worker_;
    bool running_ = false;
    bool stop_ = false;
    bool start_requested_ = false;
    bool paused_ = false;
    std::atomic<bool> cancel_current_{false};
    std::shared_ptr<const canvas::core::Project> active_project_;
    std::function<bool(const ExportSettings&, std::shared_ptr<const canvas::core::Project>&)>
        project_resolver_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> enqueued_at_;
};

}
