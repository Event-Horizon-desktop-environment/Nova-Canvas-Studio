#pragma once

// Always-on color/grade diagnostics sink for Nova Canvas Studio.
//
// A dedicated, SEPARATE log from canvas_debug.log: <home>/studio/color.log
// (override the path with CANVAS_COLOR_LOG_FILE). It captures the COLOR
// information the app gathers and the GRADING changes it applies, and unlike
// CANVAS_LOG it is NOT gated on CANVAS_DEBUG/CANVAS_PLAYBACK_DEBUG — every
// call writes unconditionally, so a default run already documents:
//
//   * per-media color spec resolution (declared tags vs the full-range probe
//     verdict), the "gather color info before anything changes" baseline;
//   * the pre-change color snapshot taken before a wheel/curve/tone-field
//     grade is committed, so the before/after of every grading action is
//     reconstructable;
//   * the grade changes themselves (commit / live preview / reset-all) with
//     the sequence token that ties them to the canvas_debug.log chain
//     (commit -> bake -> upload -> draw);
//   * the scope analysis path's matrix/range at every source-spec change.
//
// The file APPENDS across launches (unlike canvas_debug.log, which main.cpp
// truncates): color.log is an archive of color-state history, so each run is
// delimited by a [session start] line at the head. Qt-free (core/ invariant),
// thread-safe, flushed per line.

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

#include "canvas/core/util/log.hpp"

namespace canvas::core::log {

inline const char* color_log_path() {
    static const char* path = [] {
        const char* ov = std::getenv("CANVAS_COLOR_LOG_FILE");
        if (ov && *ov) {
            static std::string override_path(ov);
            return override_path.c_str();
        }
        static std::string p;
        const char* home = std::getenv("HOME");
        if (home && *home) {
            p = std::string(home) + "/studio";
#ifdef _WIN32
            _mkdir(p.c_str());
#else
            ::mkdir(p.c_str(), 0755);
#endif
            p += "/color.log";
        } else {
            p = "color.log";
        }
        return p.c_str();
    }();
    return path;
}

// Unconditional color/grade line: `[HH:MM:SS.mmm] e=<epoch_ms> <msg>`.
inline void color_log(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    static FILE* f = [] {
        const char* p = std::getenv("CANVAS_COLOR_LOG_FILE");
        if (!p || !*p) p = color_log_path();
        FILE* h = std::fopen(p, "a");
        if (h) {
            const auto now = std::chrono::system_clock::now();
            const auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tmv;
            localtime_r(&t, &tmv);
            char start[48];
            std::snprintf(start, sizeof(start), "%04d-%02d-%02d %02d:%02d:%02d",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                          tmv.tm_min, tmv.tm_sec);
            std::fprintf(h, "\n[session start %s]\n", start);
            std::fflush(h);
        }
        return h;
    }();
    if (!f) return;

    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min,
                  tmv.tm_sec,
                  static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now.time_since_epoch()).count() % 1000));

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::fprintf(f, "[%s] e=%llu %s\n", ts,
                 static_cast<unsigned long long>(epoch_ms()), buf);
    std::fflush(f);
}

}  // namespace canvas::core::log

// Format-and-write a color/grade archive line (always-on, no gate). Usage:
//     CANVAS_COLOR_LOG("[media] open path=%s resolved matrix=%s range=%s", ...)
#define CANVAS_COLOR_LOG(fmt, ...) ::canvas::core::log::color_log(fmt, ##__VA_ARGS__)