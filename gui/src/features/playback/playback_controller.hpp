#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "canvas/core/media/frame.hpp"
#include "canvas/core/media/frame_cache.hpp"
#include "canvas/core/media/video_decoder.hpp"

Q_DECLARE_METATYPE(std::shared_ptr<const canvas::core::VideoFrame>)

namespace canvas::gui {

class PlaybackController final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    void open_file(const QString& path);
    void close_media();
    void play();
    void pause();
    void toggle_play_pause();
    void seek(int64_t frame_number);
    void step(int delta);

    [[nodiscard]] bool is_playing() const { return playing_.load(); }
    [[nodiscard]] bool has_media() const { return has_media_.load(); }
    [[nodiscard]] int64_t total_frames() const { return total_frames_.load(); }
    [[nodiscard]] double fps() const { return fps_.load(); }
    [[nodiscard]] int64_t current_frame() const { return current_frame_.load(); }

signals:
    void media_opened(int64_t total_frames, double fps, int width, int height);
    void media_closed();
    void open_failed(const QString& reason);
    void frame_ready(std::shared_ptr<const canvas::core::VideoFrame> frame);
    void position_changed(int64_t frame_number);
    void playback_changed(bool playing);

private:
    enum class Command { Open, Close, Play, Pause, Seek, Step, Stop };
    struct Request {
        Command command = Command::Stop;
        int64_t arg = 0;
        QString text;
    };

    void worker_loop();
    void push(Request request);
    void handle_open(const QString& path);
    void handle_close();
    void handle_play();
    void handle_seek(int64_t frame_number);
    void present_next();

    canvas::core::VideoDecoder decoder_;
    canvas::core::FrameCache cache_;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;

    std::atomic<bool> playing_{false};
    std::atomic<bool> has_media_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int64_t> current_frame_{-1};
    std::atomic<int64_t> total_frames_{-1};
    std::atomic<double> fps_{0.0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point next_present_{};
};

}
