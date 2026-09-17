#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace canvas::core::transcript {

struct Segment {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::string text;
};

[[nodiscard]] std::string srt_timestamp(int64_t ms);

[[nodiscard]] std::string clean_segment_text(std::string_view raw);

[[nodiscard]] std::string srt_join(const std::vector<Segment>& segments);

bool write_srt(std::string_view path, const std::vector<Segment>& segments);

}
