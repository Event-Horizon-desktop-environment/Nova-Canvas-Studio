#pragma once

#include "canvas/core/timeline/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core::title {

inline constexpr float kSizeMin = 0.02f;
inline constexpr float kSizeMax = 0.5f;
inline constexpr float kSizeDefault = 0.1f;

[[nodiscard]] float clamp_size(float size) noexcept;

[[nodiscard]] std::string find_font_path();

struct FontFace {
    std::string family;
    std::string path;
};

[[nodiscard]] std::vector<FontFace> installed_font_faces();

[[nodiscard]] std::vector<std::string> installed_font_families();

[[nodiscard]] std::string find_font_path_for(const std::string& family);

[[nodiscard]] int glyph_height(const Clip& clip, int frame_height) noexcept;

struct Layout {
    int width = 0;
    int height = 0;
};

[[nodiscard]] Layout measure(const std::string& text, int glyph_height_px,
                             const std::string& font_path);

inline constexpr float kFitMaxWidthFraction = 0.94f;
inline constexpr float kFitMaxHeightFraction = 0.30f;
inline constexpr float kFitBottomMarginFraction = 0.05f;

struct SubtitleFit {
    float size = kSizeDefault;
    int block_width = 0;
    int block_height = 0;
    double pos_y = 0;
};

[[nodiscard]] SubtitleFit fit_caption(const std::string& text, int frame_width,
                                      int frame_height, const std::string& font_path,
                                      float size_hint = kSizeDefault);

void render_clip_title(const Clip& clip, std::vector<uint8_t>& rgba, int width,
                       int height, std::size_t stride);

void render_clip_title_with_font(const Clip& clip, const std::string& font_path,
                                 std::vector<uint8_t>& rgba, int width, int height,
                                 std::size_t stride);

struct TitleSprite {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int ox = 0;
    int oy = 0;
    [[nodiscard]] bool valid() const noexcept {
        return !data.empty() && width > 0 && height > 0;
    }
};

[[nodiscard]] TitleSprite raster_title_sprite(const Clip& clip, int canvas_w,
                                              int canvas_h, const std::string& font_path);

}