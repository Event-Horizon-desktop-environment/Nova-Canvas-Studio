#include "features/thumbnails/thumbnail_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QString>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace canvas::gui;

namespace {

std::string fnv1a_hex(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) out.push_back(hex[(h >> (60 - i * 4)) & 0xF]);
    return out;
}

QString thumb_disk_path(const QString& cache_dir, const std::string& path, int64_t frame,
                        int width) {
    const std::string seed =
        "t2|" + path + "|" + std::to_string(frame) + "|" + std::to_string(width);
    return cache_dir + QLatin1Char('/') + QString::fromStdString(fnv1a_hex(seed)) +
           QLatin1String(".png");
}

bool wait_for(QCoreApplication& app, const std::function<bool()>& predicate, int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count() > timeout_ms) {
            return false;
        }
        app.processEvents(QEventLoop::AllEvents, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool same_pixels(const QImage& a, const QImage& b) {
    const QImage ca = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage cb = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (ca.size() != cb.size() || ca.bytesPerLine() != cb.bytesPerLine()) return false;
    return std::memcmp(ca.constBits(), cb.constBits(),
                       static_cast<std::size_t>(ca.bytesPerLine()) *
                           static_cast<std::size_t>(ca.height())) == 0;
}

}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    int failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        if (!ok) {
            failures += 1;
            std::printf("FAIL: %s\n", what);
        }
    };

    const QString cache_dir = QDir::temp().filePath(
        QStringLiteral("canvas_thumbs_svc_test_%1").arg(QCoreApplication::applicationPid()));
    QDir(cache_dir).removeRecursively();

    ThumbnailService service;
    service.set_cache_dir(cache_dir);

    std::vector<std::pair<uint64_t, QImage>> deliveries;
    bool inside_request = false;
    bool sync_delivery_seen = false;
    QObject::connect(&service, &ThumbnailService::thumbnail_ready,
                     [&](uint64_t id, const QImage& img) {
                         deliveries.push_back({id, img});
                         if (inside_request) sync_delivery_seen = true;
                     });
    const auto request = [&](uint64_t id, const std::string& path, int64_t frame, int width) {
        ThumbRequest req;
        req.id = id;
        req.path = path;
        req.frame = frame;
        req.target_width = width;
        req.max_height = 36;
        inside_request = true;
        service.request(req);
        inside_request = false;
    };
    const auto got_id = [&](uint64_t id) {
        for (const auto& d : deliveries)
            if (d.first == id) return true;
        return false;
    };
    const auto image_for = [&](uint64_t id) -> QImage {
        for (const auto& d : deliveries)
            if (d.first == id) return d.second;
        return QImage();
    };

    {
        const std::string path = "/nonexistent/ncs/fake_media_a.mov";
        const int64_t frame = 42;
        const int width = 64;
        QImage written(width, 36, QImage::Format_ARGB32);
        written.fill(QColor(205, 30, 45));
        QPainter p(&written);
        p.setPen(QPen(Qt::white, 2));
        p.drawLine(0, 0, width - 1, 35);
        p.end();
        const QString file = thumb_disk_path(cache_dir, path, frame, width);
        expect("cache-file name is non-empty", !file.isEmpty());
        expect("disk seed file written", written.save(file, "PNG"));

        request(1, path, frame, width);
        const bool delivered = wait_for(app, [&] { return got_id(1); }, 8000);
        expect("disk-seeded request delivered (worker disk fallback)", delivered);
        const QImage got = image_for(1);
        expect("disk-seeded delivery non-null", !got.isNull());
        expect("disk-seeded delivery is EXACTLY the cached PNG", same_pixels(got, written));
    }

    {
        const std::string path = "/nonexistent/ncs/fake_media_a.mov";
        const int64_t frame = 42;
        const int width = 64;
        sync_delivery_seen = false;
        const std::size_t before = deliveries.size();
        request(2, path, frame, width);
        expect("second request delivered synchronously from memory", sync_delivery_seen);
        expect("second request produced a delivery", deliveries.size() == before + 1);
        expect("memory-served delivery is the same PNG", same_pixels(image_for(2), image_for(1)));
    }

    {
        const std::string path = "/nonexistent/ncs/fake_media_b.mov";
        const int64_t frame = 7;
        const int width = 32;
        expect("no disk file exists for the failing path",
               !QFileInfo::exists(thumb_disk_path(cache_dir, path, frame, width)));
        request(101, path, frame, width);
        request(102, path, frame, width);
        const bool drained = wait_for(app, [&] { return got_id(101) && got_id(102); }, 8000);
        expect("both parked waiters drained after total failure", drained);
        expect("waiter 101 gets a null QImage (drain signal)", image_for(101).isNull());
        expect("waiter 102 gets a null QImage (drain signal)", image_for(102).isNull());

        request(103, path, frame, width);
        const bool reenqueued = wait_for(app, [&] { return got_id(103); }, 8000);
        expect("request after a failed pass is re-enqueued, not parked (got a serve)",
               reenqueued);
        expect("re-enqueued request drains null too", image_for(103).isNull());
    }

    std::printf(failures == 0 ? "ALL THUMBNAIL-SERVICE WORKER TESTS PASSED\n"
                              : "%d THUMBNAIL-SERVICE WORKER TEST(S) FAILED\n",
                failures);
    return failures == 0 ? 0 : 1;
}
