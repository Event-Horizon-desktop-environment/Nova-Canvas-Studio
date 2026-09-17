#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/audio_waveform.hpp"

namespace canvas::core {
class VideoDecoder;
}

namespace canvas::gui {

struct ThumbRequest {
    uint64_t id = 0;
    std::string path;
    int64_t frame = 0;
    int target_width = 0;
    int max_height = 0;
    bool is_audio = false;
    float src_lo = 0.0f;
    float src_hi = 1.0f;
    float gain = 1.0f;
    int64_t src_in = 0;
    int64_t src_out = 0;
    int64_t tl_in = 0;
    int64_t tl_out = 0;
    double media_fps = 0.0;
    int64_t media_total_frames = 0;
};

class ThumbnailService final : public QObject {
    Q_OBJECT

public:
    explicit ThumbnailService(QObject* parent = nullptr);
    ~ThumbnailService() override;

    void request(ThumbRequest req);
    void request_waveform(uint64_t id, std::string path, int width, int height, float src_lo,
                          float src_hi, float gain = 1.0f, int64_t src_in = 0, int64_t src_out = 0,
                          int64_t tl_in = 0, int64_t tl_out = 0, double media_fps = 0.0,
                          int64_t media_total_frames = 0);
    void clear_cache();
    void set_cache_dir(QString dir);
    void set_paused(bool paused);

signals:
    void thumbnail_ready(uint64_t id, QImage image);
    void waveform_ready(uint64_t id, QImage image);

private:
    struct CacheKey {
        std::string path;
        int64_t frame = 0;
        int width = 0;
        bool is_audio = false;
        float src_lo = 0.0f;
        float src_hi = 1.0f;
        int gain_pct = 100;

        bool operator==(const CacheKey& o) const noexcept {
            return path == o.path && frame == o.frame && width == o.width && is_audio == o.is_audio &&
                   src_lo == o.src_lo && src_hi == o.src_hi && gain_pct == o.gain_pct;
        }
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& k) const noexcept {
            std::size_t h = std::hash<std::string>{}(k.path);
            h ^= static_cast<std::size_t>(k.frame) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.width) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.is_audio) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.src_lo) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.src_hi) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<std::size_t>(k.gain_pct) * 0x9E3779B97F4A7C15ULL;
            return h;
        }
    };

    void create_workers();
    void worker_loop();
    void submit(ThumbRequest req);
    QImage generate(const ThumbRequest& req, canvas::core::HwDeviceManager& hw,
                    canvas::core::VideoDecoder* reuse_decoder = nullptr,
                    int64_t* served_frame = nullptr);
    void store_cached(const CacheKey& key, const QImage& img);

    QString disk_path_thumbnail(const std::string& path, int64_t frame, int width) const;
    QString disk_path_waveform(const std::string& path, int width, int height, float src_lo,
                               float src_hi, int gain_pct = 100) const;
    QString disk_path_raw_waveform(const std::string& path) const;
    static QString cache_file_name(const std::string& seed, const char* ext);
    static QString ensure_cache_dir(const QString& dir);
    QImage load_from_disk(const QString& file) const;
    void save_to_disk(const QString& file, const QImage& img) const;
    bool load_raw_waveform(const QString& file, canvas::core::AudioWaveform* out) const;
    void save_raw_waveform(const QString& file, const canvas::core::AudioWaveform& wf) const;

    static constexpr int kWorkerThreads = 4;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ThumbRequest> queue_;
    std::deque<CacheKey> lru_;
    std::unordered_map<CacheKey, QImage, CacheKeyHash> cache_;
    std::unordered_map<CacheKey, std::vector<uint64_t>, CacheKeyHash> pending_ids_;
    static constexpr std::size_t kCacheMax = 1024;
    QString cache_dir_;

    static constexpr std::size_t kWaveformRawBuckets = 16384;
    mutable std::mutex waveform_mutex_;
    std::unordered_map<std::string, canvas::core::AudioWaveform> waveform_cache_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> paused_{false};
    bool pending_ = false;
};

}
