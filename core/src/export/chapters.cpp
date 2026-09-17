#include "canvas/core/export/chapters.hpp"

#include "canvas/core/timeline/markers.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace canvas::core::chapters {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}

bool container_supports(const std::string_view format) {
    const std::string f = lower(std::string(format));
    return f == "mp4" || f == "m4v" || f == "mov" || f == "quicktime" ||
           f == "matroska" || f == "mkv" || f == "webm";
}

std::vector<ExportChapter> for_export(const Sequence& seq, const bool enabled) {
    std::vector<ExportChapter> out;
    if (!enabled) return out;

    const std::vector<markers::Chapter> marks = markers::chapters_from(seq);
    if (marks.empty()) return out;

    const double duration =
        seq.fps > 0.0 ? static_cast<double>(seq.duration_frames()) / seq.fps : 0.0;

    out.reserve(marks.size());
    for (std::size_t i = 0; i < marks.size(); ++i) {
        ExportChapter c;
        c.start_seconds = marks[i].seconds;
        c.title = marks[i].label;
        const double next = i + 1 < marks.size() ? marks[i + 1].seconds : duration;
        c.end_seconds = std::max(c.start_seconds, next);
        out.push_back(std::move(c));
    }
    return out;
}

void apply(ExportSettings& es, const Sequence& seq, const bool enabled) {
    if (enabled && container_supports(es.format))
        es.chapters = for_export(seq, true);
    else
        es.chapters.clear();
}

}
