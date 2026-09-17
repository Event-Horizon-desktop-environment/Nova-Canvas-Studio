#include "canvas/core/timeline/captions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace canvas::core::captions {

const std::vector<Preset>& presets() noexcept {
    static const std::vector<Preset> kPresets{
        {"standard", {42, 1, 0}},
        {"classic", {42, 2, 2}},
        {"social", {32, 1, 1}},
        {"burned", {58, 2, 0}},
    };
    return kPresets;
}

const Options& preset_options(std::string_view name) noexcept {
    for (const auto& p : presets()) {
        if (p.name == name) return p.options;
    }
    return presets().front().options;
}

Options sanitize(const Options& opts) noexcept {
    Options out = opts;
    out.max_chars_per_line = std::clamp(out.max_chars_per_line, 4, 160);
    out.max_lines = std::clamp(out.max_lines, 1, 3);
    out.gap_frames = std::max<int64_t>(0, out.gap_frames);
    return out;
}

namespace {

[[nodiscard]] std::vector<std::string> wrap_into_lines(std::string_view text,
                                                       int max_chars) {
    std::vector<std::string> words;
    {
        std::string cur;
        const auto push_word = [&] {
            if (!cur.empty()) {
                words.push_back(std::move(cur));
                cur.clear();
            }
        };
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                push_word();
            } else {
                cur.push_back(c);
            }
        }
        push_word();
    }

    std::vector<std::string> lines;
    for (std::string& word : words) {
        if (lines.empty()) {
            lines.push_back(std::move(word));
            continue;
        }
        const int seam = static_cast<int>(lines.back().size()) + 1 +
                         static_cast<int>(word.size());
        if (seam <= max_chars) {
            lines.back() += ' ';
            lines.back() += word;
        } else {
            lines.push_back(std::move(word));
        }
    }
    if (lines.empty()) lines.push_back(std::string());
    return lines;
}

[[nodiscard]] std::vector<std::string> chunk_text(std::string_view text,
                                                  int max_chars, int max_lines) {
    std::vector<std::string> words;
    {
        std::string cur;
        const auto push_word = [&] {
            if (!cur.empty()) {
                words.push_back(std::move(cur));
                cur.clear();
            }
        };
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                push_word();
            } else {
                cur.push_back(c);
            }
        }
        push_word();
    }

    std::vector<std::string> chunks;
    std::vector<std::string> lines;
    for (std::string& word : words) {
        if (lines.empty()) {
            lines.push_back(std::move(word));
            continue;
        }
        const int seam = static_cast<int>(lines.back().size()) + 1 +
                         static_cast<int>(word.size());
        if (seam <= max_chars) {
            lines.back() += ' ';
            lines.back() += word;
            continue;
        }
        if (static_cast<int>(lines.size()) < max_lines) {
            lines.push_back(std::move(word));
            continue;
        }
        std::string caption;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) caption.push_back('\n');
            caption += lines[i];
        }
        chunks.push_back(std::move(caption));
        lines.clear();
        lines.push_back(std::move(word));
    }
    if (lines.empty() && words.empty()) return {};
    if (!lines.empty()) {
        std::string caption;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) caption.push_back('\n');
            caption += lines[i];
        }
        chunks.push_back(std::move(caption));
    }
    return chunks;
}

}

std::string wrap_line(std::string_view text, int max_chars) {
    const int width = std::max(1, max_chars);
    std::string out;
    for (const std::string& line : wrap_into_lines(text, width)) {
        if (!out.empty()) out.push_back('\n');
        out += line;
    }
    return out;
}

std::vector<Caption> shape_captions(const std::vector<transcript::Segment>& segments,
                                    const Options& raw, double sequence_fps) {
    const Options opts = sanitize(raw);
    const double fps = sequence_fps > 0.0 ? sequence_fps : 30.0;
    const int64_t gap_ms =
        static_cast<int64_t>(std::llround(static_cast<double>(opts.gap_frames) / fps * 1000.0));

    struct Chunk {
        std::string text;
        int weight = 0;
    };

    std::vector<Caption> shaped;
    shaped.reserve(segments.size());
    for (const transcript::Segment& seg : segments) {
        const std::string text = transcript::clean_segment_text(seg.text);
        if (text.empty() || seg.end_ms <= seg.start_ms) continue;

        std::vector<std::string> raw_chunks = chunk_text(text, opts.max_chars_per_line, opts.max_lines);
        if (raw_chunks.empty()) continue;

        std::vector<Chunk> chunks;
        chunks.reserve(raw_chunks.size());
        for (std::string& c : raw_chunks) {
            const int weight = static_cast<int>(c.size());
            chunks.push_back({std::move(c), weight});
        }
        const int total = [&chunks] {
            int sum = 0;
            for (const Chunk& c : chunks) sum += std::max(1, c.weight);
            return sum;
        }();
        const int64_t begin = seg.start_ms;
        const int64_t span = seg.end_ms - seg.start_ms;
        int64_t cursor = begin;
        int64_t acc = 0;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const int64_t w = std::max<int64_t>(1, chunks[i].weight);
            acc += w;
            const bool last = (i + 1 == chunks.size());
            const int64_t end = last ? seg.end_ms : begin + llround(span * acc / total);
            shaped.push_back({cursor, end, std::move(chunks[i].text)});
            cursor = end;
        }
    }

    if (gap_ms <= 0) return shaped;

    std::vector<Caption> out;
    out.reserve(shaped.size());
    int64_t prev_end = std::numeric_limits<int64_t>::min();
    for (const Caption& c : shaped) {
        int64_t start = c.start_ms;
        if (prev_end != std::numeric_limits<int64_t>::min())
            start = std::max<int64_t>(start, prev_end + gap_ms);
        if (start > c.end_ms) start = c.end_ms;
        out.push_back({start, c.end_ms, c.text});
        prev_end = c.end_ms;
    }
    return out;
}

}
