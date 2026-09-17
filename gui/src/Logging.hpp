#pragma once

#include <QDateTime>
#include <QThread>
#include <QString>
#include <QtGlobal>

#include "canvas/core/util/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace canvas::gui {

inline bool debug_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

inline bool playback_debug() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_PLAYBACK_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

inline const char* log_file_path() {
    static const char* path = [] {
        const char* p = std::getenv("CANVAS_LOG_FILE");
        if (!p || !*p) p = ::canvas::core::log::default_log_path();
        static std::string s(p);
        return s.c_str();
    }();
    return path;
}

inline void reset_log_file() {
    canvas::core::log::reset_file();
    canvas::core::log::reset_route_files();
    std::remove(log_file_path());
    for (canvas::core::log::Route r : {canvas::core::log::Route::Video,
                                       canvas::core::log::Route::Render,
                                       canvas::core::log::Route::Ux,
                                       canvas::core::log::Route::Playback,
                                       canvas::core::log::Route::Timeline,
                                       canvas::core::log::Route::Transition,
                                       canvas::core::log::Route::Color,
                                       canvas::core::log::Route::Thumbs,
                                       canvas::core::log::Route::Audio,
                                       canvas::core::log::Route::SourcePreview,
                                       canvas::core::log::Route::Project})
        std::remove(canvas::core::log::route_log_path(r));
}

inline void message_handler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const bool verbose = (type == QtDebugMsg || type == QtInfoMsg);
    if (verbose && !debug_enabled()) return;

    const char* sev = type == QtDebugMsg     ? "D"
                      : type == QtInfoMsg    ? "I"
                      : type == QtWarningMsg ? "W"
                      : type == QtCriticalMsg ? "C"
                                              : "F";
    const quint64 tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
    const QByteArray utf8 = msg.toUtf8();
    const QByteArray line = QString("[eh-gui %1 %2 th=%3] %4")
                                .arg(QLatin1String(sev),
                                     QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                                .arg(tid)
                                .arg(QString::fromUtf8(utf8))
                                .toUtf8();

    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stderr);
    std::fputc('\n', stderr);
    std::lock_guard<std::mutex> lk_(::canvas::core::log::mutex());
    if (FILE* f = ::canvas::core::log::file_for(utf8.constData()); f) {
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), f);
        std::fputc('\n', f);
        std::fflush(f);
    }
}

inline void install_logging() { qInstallMessageHandler(message_handler); }

}
