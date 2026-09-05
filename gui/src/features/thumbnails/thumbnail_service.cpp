#include "thumbnail_service.hpp"

#include "Logging.hpp"

#include <QColor>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QSize>
#include <QStandardPaths>

#include <chrono>

#include "canvas/core/media/audio_waveform.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"

namespace canvas::gui {

namespace {
// Simple FNV-1a over the seed bytes -> a compact hex string used as a stable
// on-disk filename. This avoids storing awkward characters (slashes, spaces)
// from media paths in the filesystem.
std::string fnv1a_hex(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) {
        out.push_back(hex[(h >> (60 - i * 4)) & 0xF]);
    }
    return out;
}
}  // namespace

void ThumbnailService::set_cache_dir(QString dir) {
    QMutexLocker lock(&mutex_);
    if (dir.isEmpty()) {
        cache_dir_.clear();
        return;
    }
    cache_dir_ = ensure_cache_dir(dir);
}

QString ThumbnailService::ensure_cache_dir(const QString& dir) {
    if (dir.isEmpty()) return QString();
    const QString base = QDir::fromNativeSeparators(dir);
    if (!QDir().mkpath(base)) return QString();
    return base;
}

QString ThumbnailService::cache_file_name(const std::string& seed, const char* ext) {
    return QString::fromStdString(fnv1a_hex(seed)) + QLatin1Char('.') + QLatin1String(ext);
}

QString ThumbnailService::disk_path_thumbnail(const std::string& path, int64_t frame, int width) const {
    if (cache_dir_.isEmpty()) return QString();
    const std::string seed = path + "|" + std::to_string(frame) + "|" + std::to_string(width);
    return cache_dir_ + QLatin1Char('/') + cache_file_name(seed, "png");
}

QString ThumbnailService::disk_path_waveform(const std::string& path, int width, int height,
                                             float src_lo, float src_hi) const {
    if (cache_dir_.isEmpty()) return QString();
    const std::string seed = "wf|" + path + "|" + std::to_string(width) + "|" +
                             std::to_string(height) + "|" + std::to_string(src_lo) + "|" +
                             std::to_string(src_hi);
    return cache_dir_ + QLatin1Char('/') + cache_file_name(seed, "png");
}

QString ThumbnailService::disk_path_raw_waveform(const std::string& path) const {
    if (cache_dir_.isEmpty()) return QString();
    const std::string seed = "raw2|" + path;
    return cache_dir_ + QLatin1Char('/') + cache_file_name(seed, "ehwf");
}

QImage ThumbnailService::load_from_disk(const QString& file) const {
    if (file.isEmpty()) return QImage();
    QImage img(file);
    if (img.isNull()) {
        if (debug_enabled())
            qDebug() << "thumb: disk load MISS file=" << QFileInfo(file).fileName();
        return QImage();
    }
    if (debug_enabled())
        qDebug() << "thumb: disk load HIT file=" << QFileInfo(file).fileName();
    return img;
}

void ThumbnailService::save_to_disk(const QString& file, const QImage& img) const {
    if (file.isEmpty() || img.isNull()) return;
    img.save(file, "PNG");
}

bool ThumbnailService::load_raw_waveform(const QString& file, canvas::core::AudioWaveform* out) const {
    if (file.isEmpty()) return false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream ds(&f);
    quint32 n = 0;
    ds >> n;
    if (ds.status() != QDataStream::Ok || n == 0 || n > 1'000'000) return false;
    out->peak.resize(n);
    out->rms.resize(n);
    qint64 i = 0;
    for (quint32 k = 0; k < n; ++k) {
        float p = 0, r = 0;
        ds >> p >> r;
        if (ds.status() != QDataStream::Ok) return false;
        out->peak[i] = p;
        out->rms[i] = r;
        ++i;
    }
    ds >> out->duration_seconds;
    out->buckets = static_cast<std::size_t>(i);
    if (ds.status() != QDataStream::Ok) return false;
    return true;
}

void ThumbnailService::save_raw_waveform(const QString& file, const canvas::core::AudioWaveform& wf) const {
    if (file.isEmpty()) return;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream ds(&f);
    const quint32 n = static_cast<quint32>(wf.peak.size());
    ds << n;
    for (std::size_t k = 0; k < wf.peak.size(); ++k) {
        ds << wf.peak[k] << wf.rms[k];
    }
    ds << wf.duration_seconds;
}

ThumbnailService::ThumbnailService(QObject* parent) : QObject(parent) {
    create_workers();
}

ThumbnailService::~ThumbnailService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

void ThumbnailService::create_workers() {
    for (int i = 0; i < kWorkerThreads; ++i)
        workers_.emplace_back([this] { worker_loop(); });
}

void ThumbnailService::request(ThumbRequest req) {
    if (req.path.empty() || req.target_width <= 0) return;
    submit(std::move(req));
}

void ThumbnailService::request_waveform(uint64_t id, std::string path, int width, int height,
                                        float src_lo, float src_hi) {
    if (path.empty() || width <= 0 || height <= 0) return;
    if (!(src_lo < src_hi) || src_lo >= 1.0f || src_hi <= 0.0f) { src_lo = 0.0f; src_hi = 1.0f; }
    ThumbRequest req;
    req.id = id;
    req.path = std::move(path);
    req.frame = 0;
    req.target_width = width;
    req.max_height = height;
    req.is_audio = true;
    req.src_lo = src_lo;
    req.src_hi = src_hi;
    submit(std::move(req));
}

void ThumbnailService::submit(ThumbRequest req) {
    const bool audio = req.is_audio;
    {
        QMutexLocker lock(&mutex_);
        const CacheKey key{req.path, req.frame, req.target_width, audio, req.src_lo, req.src_hi};
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            const QImage img = it->second;
            if (debug_enabled())
                qDebug() << "thumb: cache-hit id=" << req.id
                         << (audio ? "waveform" : "thumb")
                         << "path=" << QString::fromStdString(req.path)
                         << "frame=" << req.frame << "w=" << req.target_width;
            if (audio) emit waveform_ready(req.id, img);
            else emit thumbnail_ready(req.id, img);
            return;
        }

        // On-disk fallback: if this entry was generated in a previous session
        // (or a previous zoom pass evicted it from the in-memory LRU) just load
        // the tiny PNG/binary instead of re-decoding the source video/audio.
        if (audio) {
            const QString file = disk_path_waveform(req.path, req.target_width, req.max_height,
                                                    req.src_lo, req.src_hi);
            QImage img = load_from_disk(file);
            if (!img.isNull()) {
                cache_[key] = img;
                lru_.push_back(key);
                emit waveform_ready(req.id, img);
                return;
            }
        } else {
            const QString file = disk_path_thumbnail(req.path, req.frame, req.target_width);
            QImage img = load_from_disk(file);
            if (!img.isNull()) {
                cache_[key] = img;
                lru_.push_back(key);
                emit thumbnail_ready(req.id, img);
                return;
            }
        }

        if (debug_enabled())
            qDebug() << "thumb: enqueue id=" << req.id
                     << (audio ? "waveform" : "thumb")
                     << "path=" << QString::fromStdString(req.path)
                     << "frame=" << req.frame << "w=" << req.target_width
                     << "h=" << req.max_height << "qlen=" << (queue_.size() + 1);
        // Audio waveform requests are cheap (one full-file decode, then cached)
        // but the first one can take ~1s. Put them at the front of the queue so
        // they're handled immediately and don't wait behind a long video
        // cell backlog — otherwise audio finishes only after all thumbnails.
        if (audio) queue_.push_front(std::move(req));
        else queue_.push_back(std::move(req));
    }
    pending_ = true;
    cv_.notify_all();
}

void ThumbnailService::clear_cache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(waveform_mutex_);
        waveform_cache_.clear();
    }
}

void ThumbnailService::worker_loop() {
    // Each worker owns its own hardware-decode device to avoid racing on the
    // lazy init and sharing a single GPU context across threads.
    canvas::core::HwDeviceManager hw;
    while (true) {
        ThumbRequest req;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || (!queue_.empty() && pending_);
            });
            if (stopping_ && queue_.empty()) return;
            if (queue_.empty()) {
                pending_ = false;
                continue;
            }
            req = std::move(queue_.front());
            queue_.pop_front();
        }

        QImage img;
        {
            const auto t0 = std::chrono::steady_clock::now();
            img = generate(req, hw);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (debug_enabled())
                qDebug() << "thumb: generate id=" << req.id
                         << (req.is_audio ? "waveform" : "thumb")
                         << "path=" << QString::fromStdString(req.path)
                         << "frame=" << req.frame << "w=" << req.target_width
                         << "ok=" << (img.isNull() ? "NO" : "yes")
                         << "took_ms=" << ms;
        }
        if (img.isNull()) continue;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const CacheKey key{req.path, req.frame, req.target_width, req.is_audio, req.src_lo, req.src_hi};
            cache_[key] = img;
            lru_.push_back(key);
            while (lru_.size() > kCacheMax) {
                const CacheKey oldest = lru_.front();
                lru_.pop_front();
                cache_.erase(oldest);
            }
        }
        if (req.is_audio) emit waveform_ready(req.id, std::move(img));
        else emit thumbnail_ready(req.id, std::move(img));
    }
}

QImage ThumbnailService::generate(const ThumbRequest& req, canvas::core::HwDeviceManager& hw) {
    if (req.is_audio) {
        // Decode the full audio file at most once per path, then re-bucket the
        // cached high-resolution peaks to the clip's pixel width. This avoids
        // re-decoding the entire (potentially huge) audio file for every audio
        // clip and at every zoom width. Guarded because all workers may decode.
        const canvas::core::AudioWaveform* cached = nullptr;
        {
            std::lock_guard<std::mutex> lock(waveform_mutex_);
            const auto hit = waveform_cache_.find(req.path);
            if (hit != waveform_cache_.end()) cached = &hit->second;
        }
        if (!cached) {
            canvas::core::AudioWaveform raw;
            bool ok = false;
            std::string error;
            // Try a persisted raw waveform from a previous run before doing the
            // expensive full-file decode again.
            const QString raw_file = disk_path_raw_waveform(req.path);
            if (load_raw_waveform(raw_file, &raw)) {
                ok = true;
                if (debug_enabled())
                    qDebug() << "thumb: waveform loaded raw from disk path="
                             << QString::fromStdString(req.path);
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                ok = canvas::core::decode_audio_waveform(req.path, kWaveformRawBuckets, &raw, &error);
                const auto t1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (debug_enabled())
                    qDebug() << "thumb: waveform full-file decode took_ms=" << ms
                             << "buckets=" << kWaveformRawBuckets
                             << "path=" << QString::fromStdString(req.path);
                save_raw_waveform(raw_file, raw);
            }
            if (!ok) {
                if (debug_enabled())
                    qWarning() << "thumb: WAVEFORM DECODE FAIL path="
                               << QString::fromStdString(req.path)
                               << "error=" << QString::fromStdString(error);
                return QImage();
            }
            if (raw.peak.empty()) {
                if (debug_enabled())
                    qWarning() << "thumb: WAVEFORM EMPTY path="
                               << QString::fromStdString(req.path);
                return QImage();
            }
            std::lock_guard<std::mutex> lock(waveform_mutex_);
            cached = &waveform_cache_.emplace(req.path, std::move(raw)).first->second;
        }
        const canvas::core::AudioWaveform wf =
            reduce_waveform(*cached, req.target_width, req.src_lo, req.src_hi);

        QImage img(req.target_width, req.max_height, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        const double cy = img.height() / 2.0;
        const double amp = std::max(1.0, cy - 2.0);
        p.setPen(QPen(QColor(240, 244, 238, 220), 1));
        const std::size_t n = std::min(wf.peak.size(), static_cast<std::size_t>(req.target_width));
        for (std::size_t i = 0; i < n; ++i) {
            const double h = std::min(1.0, static_cast<double>(wf.peak[i])) * amp;
            p.drawLine(QPointF(i + 0.5, cy - h), QPointF(i + 0.5, cy + h));
        }
        p.end();
        save_to_disk(disk_path_waveform(req.path, req.target_width, req.max_height,
                                        req.src_lo, req.src_hi),
                     img);
        return img;
    }

    canvas::core::VideoDecoder decoder;
    std::string error;
    const auto t_open0 = std::chrono::steady_clock::now();
    const bool opened = decoder.open(req.path, &error, hw.device_ctx());
    const auto t_open1 = std::chrono::steady_clock::now();
    if (!opened) {
        if (debug_enabled())
            qWarning() << "thumb: VIDEO DECODE FAIL (open) path="
                       << QString::fromStdString(req.path)
                       << "frame=" << req.frame
                       << "error=" << QString::fromStdString(error);
        return QImage();
    }

    const int64_t source_frame = std::max<int64_t>(0, req.frame);
    const auto t_dec0 = std::chrono::steady_clock::now();
    auto frame = decoder.decode_to_frame(source_frame);
    const auto t_dec1 = std::chrono::steady_clock::now();
    if (!frame || frame->rgba.empty()) {
        if (debug_enabled())
            qWarning() << "thumb: VIDEO DECODE FAIL (frame) path="
                       << QString::fromStdString(req.path)
                       << "frame=" << source_frame
                       << "got_null=" << (frame ? "no" : "yes");
        return QImage();
    }
    if (debug_enabled())
        qDebug() << "thumb: video open_ms="
                 << std::chrono::duration<double, std::milli>(t_open1 - t_open0).count()
                 << "decode_ms="
                 << std::chrono::duration<double, std::milli>(t_dec1 - t_dec0).count()
                 << "hw=" << (decoder.is_hardware() ? 1 : 0);

    QImage source(frame->rgba.data(), frame->width, frame->height,
                  static_cast<qsizetype>(frame->stride), QImage::Format_RGBA8888);
    source = source.copy();

    const double scale = static_cast<double>(req.target_width) / source.width();
    const int scaled_h = std::max(1, static_cast<int>(source.height() * scale));
    QImage result;
    if (req.max_height > 0 && scaled_h > req.max_height) {
        const double hs = static_cast<double>(req.max_height) / scaled_h;
        result = source.scaled(QSize(req.target_width, req.max_height),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else {
        result = source.scaled(QSize(req.target_width, scaled_h),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    save_to_disk(disk_path_thumbnail(req.path, req.frame, req.target_width), result);
    return result;
}

}  // namespace canvas::gui
