#include "canvas/core/export/edl.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace canvas::core::edl {

namespace {

std::string basename_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// CMX reels are up to 8 alphanumeric/underscore chars, conventionally upper
// case. Strip the extension, keep [A-Z0-9_], uppercase, truncate.
std::string reel_of(const std::string& path) {
    std::string base = basename_of(path);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    std::string reel;
    for (const char ch : base) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc) || ch == '_')
            reel.push_back(static_cast<char>(std::toupper(uc)));
        if (reel.size() == 8) break;
    }
    return reel.empty() ? "AX" : reel;
}

const MediaEntry* media_entry(const std::vector<MediaEntry>& media, const MediaId id) {
    for (const auto& m : media)
        if (m.id == id) return &m;
    return nullptr;
}

struct Event {
    int64_t tl_in = 0;
    const Clip* clip = nullptr;
    bool video = true;
};

}  // namespace

std::string timecode(const int64_t frame, const int fps) {
    const int rate = fps > 0 ? fps : 30;
    const int64_t f = frame > 0 ? frame : 0;
    const int64_t total = f;
    const int64_t ff = total % rate;
    const int64_t ss = (total / rate) % 60;
    const int64_t mm = (total / (rate * 60)) % 60;
    const int64_t hh = total / (rate * 3600);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld:%02lld", static_cast<long long>(hh),
                  static_cast<long long>(mm), static_cast<long long>(ss),
                  static_cast<long long>(ff));
    return buf;
}

std::string write_cmx3600(const Sequence& seq, const std::string& title,
                          const std::vector<MediaEntry>& media) {
    // Collect every clip (video then audio, each track in order), then sort by
    // timeline in-point with a stable sort so same-frame events keep track order.
    std::vector<Event> events;
    for (const auto& t : seq.video_tracks)
        for (const auto& c : t.clips) events.push_back(Event{c.tl_in, &c, true});
    for (const auto& t : seq.audio_tracks)
        for (const auto& c : t.clips) events.push_back(Event{c.tl_in, &c, false});
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.tl_in < b.tl_in; });

    const int fps = seq.fps > 0.0 ? static_cast<int>(seq.fps + 0.5) : 30;

    std::string out;
    out += "TITLE: " + title + "\n";
    out += "FCM: NON-DROP FRAME\n\n";

    int number = 1;
    for (const Event& e : events) {
        const Clip& c = *e.clip;
        const MediaEntry* m = media_entry(media, c.media);
        const std::string reel = (c.media >= 0 && m) ? reel_of(m->path) : "AX";
        const char track = e.video ? 'V' : 'A';

        char line[160];
        std::snprintf(line, sizeof(line), "%03d  %-8s %-5c C        %s %s %s %s\n", number,
                      reel.c_str(), track, timecode(c.src_in, fps).c_str(),
                      timecode(c.src_out, fps).c_str(), timecode(c.tl_in, fps).c_str(),
                      timecode(c.tl_out, fps).c_str());
        out += line;
        if (m)
            out += "* FROM CLIP NAME: " + basename_of(m->path) + "\n";
        ++number;
    }
    return out;
}

}  // namespace canvas::core::edl