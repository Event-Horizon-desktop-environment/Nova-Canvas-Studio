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

#include <algorithm>
#include <cmath>

#include "canvas/core/media/audio_waveform.hpp"
#include "canvas/core/media/hw_device.hpp"
#include "canvas/core/media/video_decoder.hpp"

#include "features/playback/sync_constants.hpp"
#include "UX/theme.hpp"

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
                                             float src_lo, float src_hi, int gain_pct) const {
    if (cache_dir_.isEmpty()) return QString();
    // "wf2": render-format version bump — old peak-only PNGs (solid full-height
    // blocks on loud, low-dynamic-range material like heavily-limited music) are
    // invalidated by the dB-scaled RMS+peak render (see generate).
    const std::string seed = "wf2|" + path + "|" + std::to_string(width) + "|" +
                             std::to_string(height) + "|" + std::to_string(src_lo) + "|" +
                             std::to_string(src_hi) + "|g" + std::to_string(gain_pct);
    return cache_dir_ + QLatin1Char('/') + cache_file_name(seed, "png");
}

QString ThumbnailService::disk_path_raw_waveform(const std::string& path) const {
    if (cache_dir_.isEmpty()) return QString();
    const std::string seed = "raw3|" + path;
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
                                        float src_lo, float src_hi, float gain, int64_t src_in,
                                        int64_t src_out, int64_t tl_in, int64_t tl_out,
                                        double media_fps, int64_t media_total_frames) {
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
    req.gain = std::clamp(gain, 0.0f, 1.0f);
    req.src_in = src_in;
    req.src_out = src_out;
    req.tl_in = tl_in;
    req.tl_out = tl_out;
    req.media_fps = media_fps;
    req.media_total_frames = media_total_frames;
    submit(std::move(req));
}

void ThumbnailService::submit(ThumbRequest req) {
    const bool audio = req.is_audio;
    // Waveforms are content displays: they are NEVER scaled by clip volume (that
    // used to flatten them into a line at low gain and make disk/cache keys vary
    // per gain percent), so the quantized key component is a constant 100 and a
    // clip's spectrum is identical at every volume setting.
    const int gain_pct = 100;
    req.gain = 1.0f;
    {
        QMutexLocker lock(&mutex_);
        const CacheKey key{req.path, req.frame, req.target_width, audio, req.src_lo, req.src_hi,
                           gain_pct};
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
                                                    req.src_lo, req.src_hi, gain_pct);
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

void ThumbnailService::set_paused(bool paused) {
    paused_ = paused;
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
    canvas::core::HwDeviceManager hw{"thumbs"};
    while (true) {
        ThumbRequest req;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                // Parked while paused_ (an export owns the GPU/disk): requests
                // keep queueing and flush once the render clears the flag.
                return stopping_ || (!paused_ && !queue_.empty() && pending_);
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
            // Always-on throttle (~1/s): team-wide generate throughput and backlog.
            // A queue that keeps growing past kWorkers with slow took_ms means
            // imports/scrubs are feeding faster than the decode workers drain.
            static auto agg_t0 = t0;
            static int agg_n = 0;
            static double agg_ms = 0.0;
            ++agg_n;
            agg_ms += ms;
            const double since_s = std::chrono::duration<double>(t1 - agg_t0).count();
            if (since_s >= 1.0) {
                const double avg_ms = agg_ms / static_cast<double>(agg_n);
                size_t ql = 0, cs = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ql = queue_.size();
                    cs = cache_.size();
                }
                std::size_t wc = 0;
                {
                    std::lock_guard<std::mutex> lock(waveform_mutex_);
                    wc = waveform_cache_.size();
                }
                qDebug().nospace()
                    << "[thumb] generated=" << agg_n
                    << " avg_ms=" << QString::number(avg_ms, 'f', 0)
                    << " last_ms=" << QString::number(ms, 'f', 0)
                    << " queue=" << ql
                    << " lru=" << cs
                    << " wf_cache=" << wc;
                agg_t0 = t1;
                agg_n = 0;
                agg_ms = 0.0;
            }
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
                // Rare (one per unique audio file per session), so always-on: a
                // raw cached waveform makes first-paint instant; a miss means the
                // next block incurs the full-file decode.
                qDebug().nospace() << "thumb: waveform loaded RAW from disk path="
                                     << QString::fromStdString(req.path);
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                ok = canvas::core::decode_audio_waveform(req.path, kWaveformRawBuckets, &raw, &error);
                const auto t1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                // Always-on: the first waveform for a file is a full-file decode
                // (can be seconds on a long take); repeated slow ones point at a
                // painful disk or a format that defeats the cached raw.
                qDebug().nospace() << "thumb: waveform full-file DECODE took_ms="
                                     << QString::number(ms, 'f', 0)
                                     << " buckets=" << kWaveformRawBuckets
                                     << " path=" << QString::fromStdString(req.path);
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

        // Always-on cross-grid audit: the waveform buckets are placed on the
        // AUDIO-time grid (t / duration_seconds), but the clip window fractions
        // were derived from the VIDEO-frame grid (src_in / total_frames). The two
        // agree only when media_fps * total_frames == duration_seconds. Print the
        // drift in frames at both edges so a blade aimed at a drawn transient can
        // be reconciled with the audio actually at that frame. Zero cost (muted
        // unless a timeout fires), always-on by request (blade-cut debugging).
        {
            const double dur = cached->duration_seconds;
            const double lo = static_cast<double>(req.src_lo);
            const double hi = static_cast<double>(req.src_hi);
            // Audio-time window at the clip's source edges.
            const double audio_t_lo = lo * dur;
            const double audio_t_hi = hi * dur;
            // Video-frame window at the same source edges (exact only when the
            // media is CFR and the container duration matches frames/fps).
            const bool have_vid = req.media_fps > 0.0 && req.media_total_frames > 0 &&
                                  req.src_out > req.src_in;
            double video_t_lo = -1.0, video_t_hi = -1.0;
            double drift_lo = 0.0, drift_hi = 0.0;  // in frames
            if (have_vid) {
                video_t_lo = static_cast<double>(req.src_in) / req.media_fps;
                video_t_hi = static_cast<double>(req.src_out) / req.media_fps;
                drift_lo = (audio_t_lo - video_t_lo) * req.media_fps;
                drift_hi = (audio_t_hi - video_t_hi) * req.media_fps;
            }
            // Media-pool previews pass src_in/src_out = 0 (whole file), so the
            // grid cross-check only fires for real timeline clips.
            if (have_vid) {
                qDebug().nospace()
                    << "[wave] AUDIT id=" << req.id
                    << " dur=" << QString::number(dur, 'f', 3) << "s"
                    << " lo=" << QString::number(lo, 'g', 6)
                    << " hi=" << QString::number(hi, 'g', 6)
                    << " src=[" << req.src_in << "," << req.src_out << ")"
                    << " tl=[" << req.tl_in << "," << req.tl_out << ")"
                    << " fps=" << QString::number(req.media_fps, 'g', 4)
                    << " vtotal=" << req.media_total_frames
                    << " vdur=" << QString::number(req.media_total_frames / req.media_fps, 'f', 3) << "s"
                    << " audioT=[" << QString::number(audio_t_lo, 'f', 3) << ","
                    << QString::number(audio_t_hi, 'f', 3) << "]s"
                    << " videoT=[" << QString::number(video_t_lo, 'f', 3) << ","
                    << QString::number(video_t_hi, 'f', 3) << "]s"
                    << " drift_lo_f=" << QString::number(drift_lo, 'f', 2)
                    << " drift_hi_f=" << QString::number(drift_hi, 'f', 2);
            } else {
                qDebug().nospace()
                    << "[wave] AUDIT id=" << req.id << " (whole-file/legacy)"
                    << " dur=" << QString::number(dur, 'f', 3) << "s"
                    << " lo=" << QString::number(lo, 'g', 6)
                    << " hi=" << QString::number(hi, 'g', 6);
            }
        }

        QImage img(req.target_width, req.max_height, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        const double cy = img.height() / 2.0;
        // Height scale = half the row, full content amplitude. The waveform is a
        // content readout and deliberately ignores clip volume (which is told by
        // the volume line) — scaling bars by gain made quiet clips flatten into a
        // single horizontal line, i.e. the "spectrum stops showing changes" bug.
        const double amp = std::max(1.0, cy - 2.0);
        p.setPen(QPen(tokens().ink, 1));
        const std::size_t n = std::min(wf.peak.size(), static_cast<std::size_t>(req.target_width));
        // dB-scale so dynamic range survives the draw: a heavily-limited/loud
        // master (peak ~= 1.0 full-bar every bucket in a linear scale) used to
        // render as one solid full-height block — "blocky and sharp" next to a
        // quieter, higher-DR clip that looked "detailed". Mapping both peak and
        // RMS to a -40 dB..0 dB rack keeps beats/sections visible in loud music,
        // and drawing the RMS body plus a dim peak cap shows the real envelope
        // (quiet < loud < transient) instead of a single all-or-nothing spike.
        const auto amp_db = [](float x) -> double {
            if (x <= 1e-4f) return 0.0;
            const double db = 20.0 * std::log10(static_cast<double>(x));
            return std::clamp((db + 40.0) / 40.0, 0.0, 1.0);
        };
        QColor peak_dim = tokens().ink_muted;
        peak_dim.setAlpha(110);
        p.setPen(QPen(peak_dim, 1));
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) + 0.5;
            const double top = cy - amp_db(wf.peak[i]) * amp;
            const double bot = cy + amp_db(wf.peak[i]) * amp;
            const double top_r = cy - amp_db(wf.rms[i]) * amp;
            const double bot_r = cy + amp_db(wf.rms[i]) * amp;
            // dim peak cap (upper + lower)
            p.drawLine(QPointF(x, top), QPointF(x, top_r));
            p.drawLine(QPointF(x, bot_r), QPointF(x, bot));
        }
        p.setPen(QPen(tokens().ink, 1));
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) + 0.5;
            const double top_r = cy - amp_db(wf.rms[i]) * amp;
            const double bot_r = cy + amp_db(wf.rms[i]) * amp;
            p.drawLine(QPointF(x, top_r), QPointF(x, bot_r));
        }
        p.end();
        save_to_disk(disk_path_waveform(req.path, req.target_width, req.max_height,
                                        req.src_lo, req.src_hi,
                                        std::clamp(static_cast<int>(std::lround(req.gain * 100.0f)),
                                                   0, 100)),
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
    // Cap to the shared preview dimension: the timeline rescales each cell to a
    // handful of pixels (<=192 wide), and zoom-out films trip over HUNDREDS of
    // clip cells at once — a full-res frame per request swamps the 4 workers and
    // the strip stays blank for ages. 640px max-dim decodes land in ms.
    auto frame = decoder.decode_to_frame(source_frame, kPreviewMaxDim);
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
