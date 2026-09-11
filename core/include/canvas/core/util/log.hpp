#pragma once

// Minimal debug logging for the standalone core library (no Qt dependency).
//
// All core logging is gated at runtime on the environment variable CANVAS_DEBUG
// (any non-empty value other than "0" enables it). Messages are written to
// stderr and appended to a file as well. This keeps default and headless-test
// builds quiet while allowing a verbose debugging session via:
//     CANVAS_DEBUG=1 CANVAS_LOG_FILE=/tmp/eh.log ./canvas
//
// Message routing: a line whose body starts with a bracketed tag (or a known
// tag-less prefix) is routed to a per-category file in the same directory as the
// default log. Audited 2026-09-11 call-site inventory -> destination:
//     [dec]/[decode]/[media]/[hw]/[io]/[trans-bake]/[viewer]                         -> Canvas-Video.log
//       + tag-less video_decoder:/decode:/decode open:/vdecode/video:/viewer:/
//         transition: preview|playhead
//     [render]/[render:q]/[export]/[gpu]/[wrh]/[TIMING]/[FRAME-DIAG]/[AUDIO-DIAG]/[dbg]
//       + tag-less RenderSession::/render queue:/render_video_frame:/render_audio_chunk:/
//         frame_gpu:/renderer:/render failure:/render:                               -> Canvas-render.log
//     [edit]/[blade] + tag-less blade:/trim:/ripple:/lift:/delete_through_edit:/delete:/delete
//         /transition: set /timeline:                                                -> Canvas-Timeline.log
//     [play]/[playback]/[transport]/[loop]/[scrub]/[scrub:*]                         -> Canvas-Playback.log
//     [grade]/[curve]/[wheels]/[knob]/[knobmaster]/[tone]/[target]/[preview]/[graph]/
//         [page]/[ministrip]/[scope]/[hist]/[vectorscope]/[chromaticity]             -> Canvas-Color.log
//     [thumb]/[wave] + tag-less thumb:                                               -> Canvas-Thumbs.log
//     [audio]/[audio:feed]/[avsync]/[eq]/[diag:cut] + tag-less audio:/audio_decoder:/
//         audio decode:                                                              -> canvas-Audio.log
//     [proj]/[import]/[seq]/[env]/[build]/[font]/[eventloop]/[ui:*] + tag-less project:
//                                                                                    -> Canvas-UX.log
// anything else stays in canvas_debug.log. stderr always sees every line regardless
// of route, so a terminal session still shows the full interleaved picture.

#include <chrono>
#include <cstdarg>
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

<<<<<<< Updated upstream
=======
// Message category used to choose the destination log file. Lines routed to a
// category are written ONLY to that category's file (and stderr); Default keeps
// every line that doesn't match any category.
enum class Route : unsigned char { Default, Video, Render, Ux, Playback, Timeline, Color, Thumbs, Audio };

namespace tag_tables {

struct Entry {
    const char* token;
    Route route;
};

// Bracketed `[tag]` -> route, prefix-matched (e.g. "render:q" hits "render",
// "scrub:TRACE" hits "scrub", "ui:timeline" hits "ui"). Order matters: the first
// entry whose token is a prefix wins, so long/specific tokens precede short ones
// they'd collide with.
constexpr Entry kTags[] = {
    {"trans-bake", Route::Video},
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
    {"hw", Route::Video}, {"io", Route::Video}, {"viewer", Route::Video},
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
    {"audio", Route::Audio}, {"avsync", Route::Audio}, {"eq", Route::Audio},
    {"proj", Route::Ux}, {"import", Route::Ux}, {"seq", Route::Ux}, {"env", Route::Ux},
    {"build", Route::Ux}, {"font", Route::Ux}, {"eventloop", Route::Ux},
    {"ui", Route::Ux},
};

// Tag-less leading `prefix:` -> route. Same prefix-match rule; long/specific
// first (e.g. "decode open:" is checked before "decode:", "render queue:" before
// "render:").
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
    {"transition: set", Route::Timeline},
    {"transition: playhead", Route::Video},
    {"transition: preview", Route::Video},
    {"frame_gpu:", Route::Render},
    {"renderer:", Route::Render},
    {"render failure:", Route::Render},
    {"render:", Route::Render},
    {"audio:", Route::Audio},
    {"project:", Route::Ux},
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
};

}  // namespace tag_tables

inline Route route_for_tokens(const std::string_view tok, const tag_tables::Entry* table,
                              std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::string_view needle(table[i].token);
        if (tok.substr(0, needle.size()) == needle) return table[i].route;
    }
    return Route::Default;
}

// Extract the leading `[tag]` (or known tag-less `prefix:`) from a formatted log
// body and return the category it maps to, or Route::Default.
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

// Destination file path for a route, in the same directory as default_log_path().
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
    static const std::string color = dir + "Canvas-Color.log";
    static const std::string thumbs = dir + "Canvas-Thumbs.log";
    static const std::string audio = dir + "canvas-Audio.log";
    switch (r) {
        case Route::Video: return video.c_str();
        case Route::Render: return render.c_str();
        case Route::Ux: return ux.c_str();
        case Route::Playback: return playback.c_str();
        case Route::Timeline: return timeline.c_str();
        case Route::Color: return color.c_str();
        case Route::Thumbs: return thumbs.c_str();
        case Route::Audio: return audio.c_str();
        case Route::Default: break;
    }
    return default_log_path();
}

// Lazy-opened handle for a specific route's file. Default routes to file(),
// the shared all-lines log. (Forward-declared because Route::Audio routes to the
// dedicated audio sink, whose definition appears later in this header.)
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
            // Reuse the dedicated audio sink (honors CANVAS_AUDIO_LOG_FILE).
            return audio_file();
        case Route::Default:
        default:
            return file();
    }
}

// Destination handle for a formatted body: route it by leading tag.
inline FILE* file_for(const char* body) { return route_file_handle(route_of(body)); }

// Second, dedicated sink for the audio path: <home>/studio/canvas-Audio.log
// (overridable with CANVAS_AUDIO_LOG_FILE). The playback/mix/EQ seams write
// here so the audio trace is readable without the [dec] nv12 spam that drowns
// canvas_debug.log — toggle a clip's EQ on/off and the ON/OFF transitions land
// beside the [audio]/[avsync] telemetry and the mix-peak warnings, which is
// what makes a pop/static reproducible from the log alone.
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

// Close the cached log handles so the next write re-opens the files fresh.
// Called from the GUI's reset_log_file() so the core writer doesn't keep a
// stale fd to an old inode after the file is removed and recreated.
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

// Close and forget the Route-tagged handles (so reset from a new launch makes
// them lazily re-open the freshly-removed files).
inline void reset_route_files() {
    for (Route r : {Route::Video, Route::Render, Route::Ux, Route::Playback,
                    Route::Timeline, Route::Color, Route::Thumbs, Route::Audio}) {
        FILE*& h = route_file_handle(r);
        if (h) {
            std::fclose(h);
            h = nullptr;
        }
    }
}

>>>>>>> Stashed changes
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
            if (FILE* f_ = ::canvas::core::log::file_for(fmt); f_)                     \
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

<<<<<<< Updated upstream
=======
// Unconditional (always-on) informational line. Same sink/flush discipline as
// log_warning, but for periodic PERFORMANCE TELEMETRY and state transitions
// rather than anomalies: e.g. the `[grade]` apply-time snapshots and `[dec]`
// aggregates. Distinct level so a session log reads as INFO = measurements,
// WARN = conditions to notice, ERROR = failures — and the whole thing stays
// available in real time with no CANVAS_DEBUG/cmd.
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

// The [audio]/[eq]/[avsync] path is routed here: the leading tag routes the line
// to the dedicated canvas-Audio.log sink via the normal route table, so the mix
// trace stays readable without the [dec] nv12 spam. stderr still sees every line.
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
}  // namespace detail

// Unconditional audio-path line. Use for EQ on/off transitions, mix-peak
// warnings and the periodic mix telemetry — anything that must correlate with
// an audible artifact from canvas-Audio.log alone.
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

>>>>>>> Stashed changes
}  // namespace canvas::core::log
