#include "canvas/core/media/transcript.hpp"

#include <cstdio>
#include <fstream>

namespace canvas::core::transcript {

namespace {

// Shared cue-time law, also exposed as srt_timestamp() above. Round up never:
// the engine hands us sub-ms boundaries, truncate to keep cues from bleeding.
int64_t clamp_ms(int64_t ms) {
    if (ms < 0) return 0;
    if (ms >= 100 * 3600 * 1000) return 99 * 3600 * 1000 + 59 * 60 * 1000 + 59 * 1000 + 999;
    return ms;
}

void append_srt_time(std::string& out, int64_t ms) {
    const int64_t t = clamp_ms(ms);
    const int64_t hours = t / (3600 * 1000);
    const int64_t minutes = (t / (60 * 1000)) % 60;
    const int64_t seconds = (t / 1000) % 60;
    const int64_t millis = t % 1000;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(hours), static_cast<long long>(minutes),
                  static_cast<long long>(seconds), static_cast<long long>(millis));
    out += buf;
}

}  // namespace

std::string srt_timestamp(int64_t ms) {
    std::string out;
    append_srt_time(out, ms);
    return out;
}

std::string clean_segment_text(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    bool pending_space = false;
    bool started = false;
    for (const char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            if (started) pending_space = true;
            continue;
        }
        if (pending_space) {
            if (!out.empty()) out += ' ';
            pending_space = false;
        }
        out += c;
        started = true;
    }
    return out;
}

std::string srt_join(const std::vector<Segment>& segments) {
    std::string out;
    int cue = 1;
    for (const Segment& s : segments) {
        const std::string text = clean_segment_text(s.text);
        if (text.empty()) continue;
        out += std::to_string(cue) + "\r\n";
        append_srt_time(out, s.start_ms);
        out += " --> ";
        append_srt_time(out, s.end_ms);
        out += "\r\n";
        out += text;
        out += "\r\n\r\n";
        ++cue;
    }
    return out;
}

bool write_srt(std::string_view path, const std::vector<Segment>& segments) {
    std::ofstream file(std::string(path), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << srt_join(segments);
    return file.good();
}

}  // namespace canvas::core::transcript