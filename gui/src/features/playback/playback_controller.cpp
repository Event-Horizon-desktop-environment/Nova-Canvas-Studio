#include "playback_controller.hpp"

#include <algorithm>
#include <utility>

namespace canvas::gui {

using Clock = std::chrono::steady_clock;

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<std::shared_ptr<const canvas::core::VideoFrame>>();
    worker_ = std::thread([this] { worker_loop(); });
}

PlaybackController::~PlaybackController() {
    stopping_.store(true);
    {
        const std::lock_guard lock(mutex_);
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PlaybackController::push(Request request) {
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    cv_.notify_all();
}

void PlaybackController::open_file(const QString& path) {
    pause();
    push({Command::Open, 0, path});
}

void PlaybackController::close_media() { push({Command::Close, 0, {}}); }

void PlaybackController::play() { push({Command::Play, 0, {}}); }

void PlaybackController::pause() { push({Command::Pause, 0, {}}); }

void PlaybackController::toggle_play_pause() { push({is_playing() ? Command::Pause : Command::Play, 0, {}}); }

void PlaybackController::seek(const int64_t frame_number) { push({Command::Seek, frame_number, {}}); }

void PlaybackController::step(const int delta) { push({Command::Step, delta, {}}); }

void PlaybackController::worker_loop() {
    for (;;) {
        Request req;
        {
            std::unique_lock lock(mutex_);
            if (!playing_.load()) {
                cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            } else {
                cv_.wait_until(lock, next_present_, [this] { return stopping_.load() || !queue_.empty(); });
            }
            if (stopping_.load() && queue_.empty()) return;
            if (queue_.empty()) {
                present_next();
                continue;
            }
            req = std::move(queue_.front());
            queue_.pop_front();
        }
        switch (req.command) {
        case Command::Stop:
            return;
        case Command::Open:
            handle_open(req.text);
            break;
        case Command::Close:
            handle_close();
            break;
        case Command::Play:
            handle_play();
            break;
        case Command::Pause:
            playing_.store(false);
            emit playback_changed(false);
            break;
        case Command::Seek:
            handle_seek(req.arg);
            break;
        case Command::Step:
            handle_seek(current_frame_.load() + req.arg);
            break;
        }
    }
}

void PlaybackController::handle_open(const QString& path) {
    playing_.store(false);
    emit playback_changed(false);
    has_media_.store(false);
    current_frame_.store(-1);
    total_frames_.store(-1);
    fps_.store(0.0);

    std::string error;
    if (!decoder_.open(path.toStdString(), &error)) {
        cache_.clear();
        emit open_failed(QString::fromStdString(error));
        return;
    }
    cache_.clear();
    total_frames_.store(decoder_.total_frames());
    fps_.store(decoder_.frame_rate());
    has_media_.store(true);
    emit media_opened(decoder_.total_frames(), decoder_.frame_rate(), decoder_.width(), decoder_.height());
    handle_seek(0);
}

void PlaybackController::handle_close() {
    playing_.store(false);
    emit playback_changed(false);
    has_media_.store(false);
    current_frame_.store(-1);
    total_frames_.store(-1);
    fps_.store(0.0);
    cache_.clear();
    decoder_.close();
    emit media_closed();
}

void PlaybackController::handle_play() {
    if (!has_media_.load()) return;
    playing_.store(true);
    next_present_ = Clock::now();
    emit playback_changed(true);
}

void PlaybackController::handle_seek(const int64_t frame_number) {
    int64_t target = frame_number;
    const int64_t last = total_frames_.load();
    if (last > 0) target = std::clamp(target, int64_t{0}, last - 1);
    if (target < 0) target = 0;
    if (!has_media_.load()) return;

    auto frame = cache_.get(target);
    bool from_cache = static_cast<bool>(frame);
    if (!from_cache) frame = decoder_.seek_to_frame(target);
    if (!frame) return;

    current_frame_.store(frame->frame_number);
    if (!from_cache && target == frame->frame_number) cache_.put(frame);
    emit frame_ready(std::move(frame));
    emit position_changed(current_frame_.load());
}

void PlaybackController::present_next() {
    const int64_t want = current_frame_.load() + 1;
    auto frame = cache_.get(want);
    if (!frame) frame = decoder_.decode_next();
    if (!frame) {
        playing_.store(false);
        emit playback_changed(false);
        return;
    }
    const double rate = fps_.load();
    const auto interval = rate > 0.0
        ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate))
        : Clock::duration{33333};
    const auto now = Clock::now();
    if (next_present_ + interval * 4 < now) next_present_ = now;

    current_frame_.store(frame->frame_number);
    cache_.put(frame);
    emit frame_ready(std::move(frame));
    emit position_changed(current_frame_.load());

    next_present_ += interval;
}

}
