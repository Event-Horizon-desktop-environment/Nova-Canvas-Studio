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

namespace canvas::gui {

struct ThumbRequest {
    uint64_t id = 0;
    std::string path;
    int64_t frame = 0;
    int target_width = 0;
    int max_height = 0;
    bool is_audio = false;
    // For audio waveform previews: the source fraction [src_lo, src_hi) the
    // clip displays (its src_in..src_out window), so each clip renders exactly
    // its own audio and a blade cut doesn't change the visible spectrum.
    float src_lo = 0.0f;
    float src_hi = 1.0f;
    // For audio waveform previews: the clip's volume as an amplitude gain in
    // [0,1] (db_to_gain(clip.volume_db) clamped). Baked into the drawn peak
    // heights so the timeline spectrum visibly shrinks/rises with the volume.
    float gain = 1.0f;
};

class ThumbnailService final : public QObject {
    Q_OBJECT

public:
    explicit ThumbnailService(QObject* parent = nullptr);
    ~ThumbnailService() override;

    void request(ThumbRequest req);
    void request_waveform(uint64_t id, std::string path, int width, int height, float src_lo,
                          float src_hi, float gain = 1.0f);
    void clear_cache();
    // Sets the directory used to persist generated thumbnails and waveforms so
    // they survive across zooms and app restarts. Empty disables disk caching.
    void set_cache_dir(QString dir);

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
    QImage generate(const ThumbRequest& req, canvas::core::HwDeviceManager& hw);

    // Disk-cache helpers. Keyed files are written/read under cache_dir_.
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
    // In-memory LRU of decoded thumbnails/waveforms. Sized for the full
    // filmstrip across zoom passes: every cell of every clip must stay resident
    // so zooming in/out re-fills the strip from memory instead of re-decoding.
    static constexpr std::size_t kCacheMax = 1024;
    QString cache_dir_;

    // Full-resolution waveform per media path. Decoding a whole audio file is
    // expensive, so we decode it once (at a fixed high bucket count) and then
    // cheaply re-bucket to the pixel width each clip needs for its preview.
    // Accessed by all worker threads, so it is mutex-guarded.
    static constexpr std::size_t kWaveformRawBuckets = 16384;
    mutable std::mutex waveform_mutex_;
    std::unordered_map<std::string, canvas::core::AudioWaveform> waveform_cache_;

    std::atomic<bool> stopping_{false};
    bool pending_ = false;
};

}  // namespace canvas::gui
