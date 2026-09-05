#pragma once

// GUI-side debug logging for Nova Canvas Studio.
//
// A custom Qt message handler is installed by canvas::gui::install_logging() so
// that qDebug()/qWarning()/qCritical() output is written to stderr AND, when
// CANVAS_LOG_FILE is set (default "canvas_debug.log"), appended to that file with a
// severity, timestamp and thread id prefix.
//
// Gate: verbose DEBUG/INFO lines are only written when the environment variable
// CANVAS_DEBUG is set to a non-empty value other than "0". Warnings and criticals
// are always captured so crashes / Qt warnings are visible in default runs.
//     CANVAS_DEBUG=1 CANVAS_LOG_FILE=/tmp/eh.log ./canvas
//
// In application code, use the streaming form guarded by the gate, e.g.:
//     if (canvas::gui::debug_enabled()) qDebug() << "viewer: set_frame w=" << w;

#include <QDateTime>
#include <QThread>
#include <QString>
#include <QtGlobal>

#include "canvas/core/util/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace canvas::gui {

// gated(): true when CANVAS_DEBUG enables verbose core/GUI logging (runtime).
inline bool debug_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

// playback_debug(): audio + transport playback is noisy at 60fps, so its debug
// log lines are gated behind a SEPARATE flag (CANVAS_PLAYBACK_DEBUG) instead of the
// general CANVAS_DEBUG. This keeps rendering/export logs readable while playback
// diagnostics remain available on demand:
//     CANVAS_DEBUG=1 CANVAS_PLAYBACK_DEBUG=1 CANVAS_LOG_FILE=/tmp/eh.log ./canvas
inline bool playback_debug() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_PLAYBACK_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

// Resolve the destination log file path. Defaults to <home>/studio/canvas_debug.log
// (matching canvas::core::log::default_log_path()) so the GUI and core writers land
// in the same file regardless of CWD; overridable with CANVAS_LOG_FILE.
inline const char* log_file_path() {
    static const char* path = [] {
        const char* p = std::getenv("CANVAS_LOG_FILE");
        if (!p || !*p) p = ::canvas::core::log::default_log_path();
        static std::string s(p);
        return s.c_str();
    }();
    return path;
}

// Truncate (start fresh) the destination log file. Called once at app startup
// so every launch captures a clean log instead of appending to the previous
// run. Both the GUI message_handler and the core CANVAS_LOG/log_error file handle
// open in append mode, so clearing here before any logging lets every writer
// land in a fresh file for this process.
inline void reset_log_file() {
    if (FILE* h = std::fopen(log_file_path(), "w")) {
        std::fflush(h);
        std::fclose(h);
    }
}

// Qt message handler: prefixes severity/timestamp/thread and writes to the
// log file. Verbose debug/info lines are suppressed unless CANVAS_DEBUG is set.
inline void message_handler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    static FILE* f = [] {
        FILE* h = std::fopen(log_file_path(), "a");
        if (h) std::fflush(h);
        return h;
    }();

    const bool verbose = (type == QtDebugMsg || type == QtInfoMsg);
    if (verbose && !debug_enabled()) return;

    const char* sev = type == QtDebugMsg     ? "D"
                      : type == QtInfoMsg    ? "I"
                      : type == QtWarningMsg ? "W"
                      : type == QtCriticalMsg ? "C"
                                              : "F";
    const quint64 tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
    const QByteArray line = QString("[eh-gui %1 %2 th=%3] %4")
                                .arg(QLatin1String(sev),
                                     QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                                .arg(tid)
                                .arg(msg)
                                .toUtf8();

    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stderr);
    std::fputc('\n', stderr);
    if (f) {
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), f);
        std::fputc('\n', f);
        std::fflush(f);
    }
}

inline void install_logging() { qInstallMessageHandler(message_handler); }

}  // namespace canvas::gui
