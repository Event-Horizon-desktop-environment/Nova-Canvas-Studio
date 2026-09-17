#pragma once

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

}

#define CANVAS_COLOR_LOG(fmt, ...) ::canvas::core::log::color_log(fmt, ##__VA_ARGS__)
