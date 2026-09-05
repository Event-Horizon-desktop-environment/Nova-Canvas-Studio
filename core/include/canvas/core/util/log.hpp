#pragma once

// Minimal debug logging for the standalone core library (no Qt dependency).
//
// All core logging is gated at runtime on the environment variable CANVAS_DEBUG
// (any non-empty value other than "0" enables it). Messages are written to
// stderr and, when CANVAS_LOG_FILE is set (default "canvas_debug.log"), appended to
// that file as well. This keeps default and headless-test builds quiet while
// allowing a verbose debugging session via:
//     CANVAS_DEBUG=1 CANVAS_LOG_FILE=/tmp/eh.log ./canvas

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace canvas::core::log {

inline bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

// Default destination: <home>/studio/canvas_debug.log (creating the ~/studio dir if
// needed), so the app log always lands in one fixed place regardless of the CWD.
// Overridable with CANVAS_LOG_FILE.
inline const char* default_log_path() {
    static const char* path = [] {
        static std::string p;
        const char* home = std::getenv("HOME");
        if (home && *home) {
            p = std::string(home) + "/studio";
#ifdef _WIN32
            _mkdir(p.c_str());
#else
            ::mkdir(p.c_str(), 0755);
#endif
            p += "/canvas_debug.log";
        } else {
            p = "canvas_debug.log";
        }
        return p.c_str();
    }();
    return path;
}

inline FILE* file() {
    static FILE* f = [] {
        const char* path = std::getenv("CANVAS_LOG_FILE");
        if (!path || !*path) path = default_log_path();
        FILE* h = std::fopen(path, "a");
        return h;
    }();
    return f;
}

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

}  // namespace canvas::core::log

// Formats and writes a core log line. Usage: CANVAS_LOG("delete id=%lld", id);
// Expands to nothing when CANVAS_DEBUG is unset (the message itself is not
// evaluated), so it is cheap to leave in production code.
#define CANVAS_LOG(fmt, ...)                                                            \
    do {                                                                            \
        if (::canvas::core::log::enabled()) {                                           \
            std::lock_guard<std::mutex> lk_(::canvas::core::log::mutex());             \
            const auto now_ = std::chrono::system_clock::now();                    \
            const auto t_ = std::chrono::system_clock::to_time_t(now_);            \
            std::tm tmv_;                                                          \
            localtime_r(&t_, &tmv_);                                               \
            char ts_[32];                                                          \
            std::snprintf(ts_, sizeof(ts_), "%02d:%02d:%02d.%03d", tmv_.tm_hour,    \
                          tmv_.tm_min, tmv_.tm_sec,                                \
                          static_cast<int>(                                         \
                              std::chrono::duration_cast<std::chrono::milliseconds>( \
                                  now_.time_since_epoch()).count() % 1000));        \
            std::fprintf(stderr, "[eh-core %s] " fmt "\n", ts_, ##__VA_ARGS__);     \
            if (FILE* f_ = ::canvas::core::log::file(); f_)                             \
                std::fprintf(f_, "[eh-core %s] " fmt "\n", ts_, ##__VA_ARGS__);     \
        }                                                                           \
    } while (0)

// Unconditional error recording for the core engine. Unlike CANVAS_LOG (gated on
// CANVAS_DEBUG), this always writes — render failures and the export lifecycle must
// be captured even on default runs so a "Completed" with no file is debuggable.
namespace canvas::core::log {

// Unconditional (always-on) warning/status line, used by headless playback
// modules for diagnostics that qWarning used to carry (e.g. [media] open,
// [scrub] preview traces). Same sink as log_error but not an error: default
// runs capture it without CANVAS_DEBUG, like qWarning did.
inline void log_warning(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk_(::canvas::core::log::mutex());
    const auto now_ = std::chrono::system_clock::now();
    const auto t_ = std::chrono::system_clock::to_time_t(now_);
    std::tm tmv_;
    localtime_r(&t_, &tmv_);
    char ts_[32];
    std::snprintf(ts_, sizeof(ts_), "%02d:%02d:%02d.%03d", tmv_.tm_hour, tmv_.tm_min,
                  tmv_.tm_sec,
                  static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now_.time_since_epoch())
                                       .count() %
                                   1000));
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[eh-core WARN %s] %s\n", ts_, buf);
    // Flush so core lines land immediately and interleave with the GUI handler's
    // (it flushes per line); buffered stderr/file output would otherwise be lost
    // on a hard kill and make these diagnostics invisible while debugging.
    std::fflush(stderr);
    if (FILE* f_ = ::canvas::core::log::file(); f_) {
        std::fprintf(f_, "[eh-core WARN %s] %s\n", ts_, buf);
        std::fflush(f_);
    }
}

inline void log_error(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk_(::canvas::core::log::mutex());
    const auto now_ = std::chrono::system_clock::now();
    const auto t_ = std::chrono::system_clock::to_time_t(now_);
    std::tm tmv_;
    localtime_r(&t_, &tmv_);
    char ts_[32];
    std::snprintf(ts_, sizeof(ts_), "%02d:%02d:%02d.%03d", tmv_.tm_hour, tmv_.tm_min,
                  tmv_.tm_sec,
                  static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now_.time_since_epoch())
                                       .count() %
                                   1000));
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[eh-core ERROR %s] %s\n", ts_, buf);
    std::fflush(stderr);
    if (FILE* f_ = ::canvas::core::log::file(); f_) {
        std::fprintf(f_, "[eh-core ERROR %s] %s\n", ts_, buf);
        std::fflush(f_);
    }
}

}  // namespace canvas::core::log
