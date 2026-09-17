#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace canvas::core::log {

inline std::uint64_t epoch_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
}

inline bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("CANVAS_DEBUG");
        return e && *e && std::string(e) != "0";
    }();
    return on;
}

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

inline FILE*& file() {
    static FILE* f = nullptr;
    if (!f) {
        const char* path = std::getenv("CANVAS_LOG_FILE");
        if (!path || !*path) path = default_log_path();
        f = std::fopen(path, "a");
    }
    return f;
}

enum class Route : unsigned char { Default, Video, Render, Ux, Playback, Timeline, Transition, Color, Thumbs, Audio, SourcePreview, Project };

namespace tag_tables {

struct Entry {
    const char* token;
    Route route;
};

constexpr Entry kTags[] = {
    {"trans-bake", Route::Transition},
    {"transition", Route::Transition},
    {"vectorscope", Route::Color},
    {"chromaticity", Route::Color},
    {"knobmaster", Route::Color},
    {"sonicsync", Route::Playback},
    {"audio:feed", Route::Audio},
    {"diag:cut", Route::Audio},
    {"render:q", Route::Render},
    {"FRAME-DIAG", Route::Render},
    {"AUDIO-DIAG", Route::Render},
    {"TIMING", Route::Render},
    {"dec", Route::Video}, {"decode", Route::Video}, {"media", Route::Video},
    {"hw", Route::Video}, {"io", Route::Video}, {"viewer", Route::Video}, {"vaapi", Route::Video},
    {"render", Route::Render}, {"export", Route::Render}, {"gpu", Route::Render},
    {"wrh", Route::Render}, {"dbg", Route::Render},
    {"play", Route::Playback}, {"playback", Route::Playback}, {"transport", Route::Playback},
    {"loop", Route::Playback}, {"scrub", Route::Playback},
    {"edit", Route::Timeline}, {"blade", Route::Timeline},
    {"grade", Route::Color}, {"curve", Route::Color}, {"wheels", Route::Color},
    {"knob", Route::Color}, {"tone", Route::Color}, {"target", Route::Color},
    {"preview", Route::Color}, {"graph", Route::Color}, {"page", Route::Color},
    {"ministrip", Route::Color}, {"scope", Route::Color}, {"hist", Route::Color},
    {"color:scrub", Route::Color},
    {"thumb", Route::Thumbs}, {"wave", Route::Thumbs},
    {"srcprv", Route::SourcePreview}, {"pool", Route::SourcePreview},
    {"audio", Route::Audio}, {"avsync", Route::Audio}, {"eq", Route::Audio},
    {"proj", Route::Project}, {"import", Route::Ux}, {"seq", Route::Ux}, {"env", Route::Ux},
    {"build", Route::Ux}, {"font", Route::Ux}, {"eventloop", Route::Ux},
    {"ui", Route::Ux},
};

constexpr Entry kPrefixes[] = {
    {"video_decoder:", Route::Video},
    {"decode open:", Route::Video},
    {"decode:", Route::Video},
    {"render queue:", Route::Render},
    {"render_video_frame:", Route::Render},
    {"render_audio_chunk:", Route::Render},
    {"RenderSession::", Route::Render},
    {"audio_decoder:", Route::Audio},
    {"audio decode:", Route::Audio},
    {"delete_through_edit:", Route::Timeline},
    {"transition:", Route::Transition},
    {"frame_gpu:", Route::Render},
    {"renderer:", Route::Render},
    {"render failure:", Route::Render},
    {"render:", Route::Render},
    {"audio:", Route::Audio},
    {"project:", Route::Project},
    {"timeline:", Route::Timeline},
    {"blade:", Route::Timeline},
    {"trim:", Route::Timeline},
    {"ripple:", Route::Timeline},
    {"lift:", Route::Timeline},
    {"delete:", Route::Timeline},
    {"delete id=", Route::Timeline},
    {"vdecode ", Route::Video},
    {"video:", Route::Video},
    {"viewer:", Route::Video},
    {"thumb:", Route::Thumbs},
    {"srcprv:", Route::SourcePreview},
    {"pool:", Route::SourcePreview},
};

}

inline Route route_for_tokens(const std::string_view tok, const tag_tables::Entry* table,
                              std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::string_view needle(table[i].token);
        if (tok.substr(0, needle.size()) == needle) return table[i].route;
    }
    return Route::Default;
}

inline Route route_of(const char* body) {
    if (!body || !*body) return Route::Default;
    if (body[0] == '[') {
        const char* close = std::strchr(body, ']');
        if (!close) return Route::Default;
        const std::string_view tag(body + 1, static_cast<std::size_t>(close - body - 1));
        return route_for_tokens(tag, tag_tables::kTags, std::size(tag_tables::kTags));
    }
    return route_for_tokens(body, tag_tables::kPrefixes, std::size(tag_tables::kPrefixes));
}

inline const char* route_log_path(Route r) {
    static const std::string dir = [] {
        std::string p(default_log_path());
        const std::size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    }();
    static const std::string video = dir + "Canvas-Video.log";
    static const std::string render = dir + "Canvas-render.log";
    static const std::string ux = dir + "Canvas-UX.log";
    static const std::string playback = dir + "Canvas-Playback.log";
    static const std::string timeline = dir + "Canvas-Timeline.log";
    static const std::string transition = dir + "Canvas-Transition.log";
    static const std::string color = dir + "Canvas-Color.log";
    static const std::string thumbs = dir + "Canvas-Thumbs.log";
    static const std::string audio = dir + "canvas-Audio.log";
    static const std::string source = dir + "Canvas-Source.log";
    static const std::string project = dir + "project.log";
    switch (r) {
        case Route::Video: return video.c_str();
        case Route::Render: return render.c_str();
        case Route::Ux: return ux.c_str();
        case Route::Playback: return playback.c_str();
        case Route::Timeline: return timeline.c_str();
        case Route::Transition: return transition.c_str();
        case Route::Color: return color.c_str();
        case Route::Thumbs: return thumbs.c_str();
        case Route::Audio: return audio.c_str();
        case Route::SourcePreview: return source.c_str();
        case Route::Project: return project.c_str();
        case Route::Default: break;
    }
    return default_log_path();
}

inline FILE*& audio_file();
inline FILE*& route_file_handle(Route r);

inline FILE*& route_file_handle(Route r) {
    switch (r) {
        case Route::Video: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Render: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Ux: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Playback: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Timeline: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Transition: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Color: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Thumbs: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Audio:
            return audio_file();
        case Route::SourcePreview: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Project: {
            static FILE* f = nullptr;
            if (!f) f = std::fopen(route_log_path(r), "a");
            return f;
        }
        case Route::Default:
        default:
            return file();
    }
}

inline FILE* file_for(const char* body) { return route_file_handle(route_of(body)); }

inline FILE*& audio_file() {
    static FILE* f = nullptr;
    if (!f) {
        const char* path = std::getenv("CANVAS_AUDIO_LOG_FILE");
        if (!path || !*path) {
            static std::string p;
            const char* home = std::getenv("HOME");
            if (home && *home) {
                p = std::string(home) + "/studio";
#ifdef _WIN32
                _mkdir(p.c_str());
#else
                ::mkdir(p.c_str(), 0755);
#endif
                p += "/canvas-Audio.log";
            } else {
                p = "canvas-Audio.log";
            }
            path = p.c_str();
        }
        f = std::fopen(path, "a");
    }
    return f;
}

inline void reset_file() {
    if (FILE* old = file()) {
        std::fclose(old);
        file() = nullptr;
    }
    if (FILE* old = audio_file()) {
        std::fclose(old);
        audio_file() = nullptr;
    }
}

inline void reset_route_files() {
    for (Route r : {Route::Video, Route::Render, Route::Ux, Route::Playback,
                    Route::Timeline, Route::Transition, Route::Color, Route::Thumbs,
                    Route::Audio, Route::SourcePreview, Route::Project}) {
        FILE*& h = route_file_handle(r);
        if (h) {
            std::fclose(h);
            h = nullptr;
        }
    }
}

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

}

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
            if (FILE* f_ = ::canvas::core::log::file_for(fmt); f_)                     \
                std::fprintf(f_, "[eh-core %s] " fmt "\n", ts_, ##__VA_ARGS__);     \
        }                                                                           \
    } while (0)

namespace canvas::core::log {

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
    std::fflush(stderr);
    if (FILE* f_ = ::canvas::core::log::file_for(buf); f_) {
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
    if (FILE* f_ = ::canvas::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[eh-core ERROR %s] %s\n", ts_, buf);
        std::fflush(f_);
    }
}

inline void log_info(const char* fmt, ...) {
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
    std::fprintf(stderr, "[eh-core INFO %s] %s\n", ts_, buf);
    std::fflush(stderr);
    if (FILE* f_ = ::canvas::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[eh-core INFO %s] %s\n", ts_, buf);
        std::fflush(f_);
    }
}

namespace detail {
inline void write_audio(const char* level, const char* fmt, va_list* ap) {
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
    std::vsnprintf(buf, sizeof(buf), fmt, *ap);
    std::fprintf(stderr, "[eh-core %s %s] %s\n", level, ts_, buf);
    std::fflush(stderr);
    if (FILE* f_ = ::canvas::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[eh-core %s %s] %s\n", level, ts_, buf);
        std::fflush(f_);
    }
}
}

inline void log_audio_warning(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    detail::write_audio("WARN", fmt, &ap);
    va_end(ap);
}

inline void log_audio_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    detail::write_audio("INFO", fmt, &ap);
    va_end(ap);
}

}