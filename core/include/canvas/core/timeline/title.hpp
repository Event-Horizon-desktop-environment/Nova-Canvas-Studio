#pragma once

// Qt-free title-overlay rendering for title/generator clips (media < 0) and
// text burned over regular media clips. The rasteriser renders UTF-8 text with
// a TrueType/OpenType system font into an RGBA canvas using the shared
// alpha-over + opacity law, so export (renderer.cpp), playback
// (timeline_decoder.cpp) and the unit tests all draw titles identically.
//
// Font resolution is best-effort: a search of well-known Linux font paths with
// a CANVAS_TITLE_FONT override, or a named family via find_font_path_for. No
// font -> renderer draws nothing for the title (never a crash). Embedded '\n'
// breaks lines; other control characters collapse to spaces. Tracker font is
// consumed via the vendored public-domain stb_truetype header
// (core/src/timeline/stb_truetype.h) so the module stays Qt-free.

#include "canvas/core/timeline/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core::title {

// Size law: `Clip::Title::size` is the glyph em height as a fraction of the
// output frame height. Bounds mirror the Inspector's spin box range.
inline constexpr float kSizeMin = 0.02f;
inline constexpr float kSizeMax = 0.5f;
inline constexpr float kSizeDefault = 0.1f;

// Clamps a raw title size fraction to [kSizeMin, kSizeMax].
[[nodiscard]] float clamp_size(float size) noexcept;

// Resolves a TrueType/OpenType font file for title rendering. Checks the
// CANVAS_TITLE_FONT environment variable first, then the well-known Linux font
// directories. Returns an empty string when no font was found.
[[nodiscard]] std::string find_font_path();

// One installed typeface: the display family name plus the preferred file
// (the Regular face when the family ships several styles).
struct FontFace {
    std::string family;
    std::string path;
};

// Enumerates the installed TrueType/OpenType families by scanning the system
// font roots and reading each file's name table with the vendored
// stb_truetype. One entry per family, sorted by family name; the preferred
// file is the Regular style when present. Roots default to the well-known
// Linux font directories; CANVAS_TITLE_FONT_DIRS (colon-separated) overrides
// them for tests and power users. Cached for the process lifetime. Qt-free so
// the Inspector's font dropdown consumes it directly.
[[nodiscard]] std::vector<FontFace> installed_font_faces();

// Convenience view over installed_font_faces: sorted unique family names.
[[nodiscard]] std::vector<std::string> installed_font_families();

// Resolves a family name to its preferred font file (case-insensitive;
// Regular style wins). Empty input falls back to find_font_path(); an unknown
// family ALSO falls back to find_font_path() so a stale project file still
// renders instead of drawing nothing. Returns "" only when no font resolves.
[[nodiscard]] std::string find_font_path_for(const std::string& family);

// Glyph em height in pixels for a title rasterised into a `frame_height`-tall
// output (the law the renderers and the inspector share). Never <= 0.
[[nodiscard]] int glyph_height(const Clip& clip, int frame_height) noexcept;

struct Layout {
    int width = 0;
    int height = 0;
};

// Measures a title's tight pixel box at the given glyph em height, using
// `font_path`. Returns {0, 0} when the text rasterises nothing (empty line,
// unknown font, missing glyphs). Pure relative-law helper for the unit tests.
[[nodiscard]] Layout measure(const std::string& text, int glyph_height_px,
                             const std::string& font_path);

// Safe-area fractions for fit_caption's "always fits the frame" law: the
// caption block may occupy at most this fraction of frame width / height, and
// its bottom edge is pinned kFitBottomMarginFraction of the frame height above
// the frame's bottom (the classic subtitle margin).
inline constexpr float kFitMaxWidthFraction = 0.94f;
inline constexpr float kFitMaxHeightFraction = 0.30f;
inline constexpr float kFitBottomMarginFraction = 0.05f;

// The result of fitting a caption into a frame (see fit_caption).
struct SubtitleFit {
    float size = kSizeDefault;  // chosen glyph size fraction of frame height
    int block_width = 0;        // measured tight block (0 when unmeasurable)
    int block_height = 0;       // measured tight block (0 when unmeasurable)
    double pos_y = 0;           // px offset from centre (down positive) that pins
                                // the block's bottom at the bottom safe margin
};

// Fits a caption's text so it ALWAYS lands inside the frame, bottom-anchored
// like a real subtitle: returns the largest glyph size fraction <= `size_hint`
// (clamped into the [kSizeMin, kSizeMax] law) whose stacked block fits the
// fit_caption width/height safe areas, plus the pos_y that pins it just above
// the frame bottom. Uses the same tight-box + 0.25 em line-stacking math as the
// rasteriser, so what fits here is exactly what renders. When the text or font
// is unusable (empty text, no font path) the hint survives and pos_y falls back
// to a nominal bottom offset (no measured block — block_* are 0).
[[nodiscard]] SubtitleFit fit_caption(const std::string& text, int frame_width,
                                      int frame_height, const std::string& font_path,
                                      float size_hint = kSizeDefault);

// Draws `clip.title` over the RGBA canvas (stride bytes per row, opaque alpha
// out). The title block is centred both axes, then shifted by the clip's
// visual position (pos_x/pos_y, output px — zero by default); glyph height
// follows the size law; embedded '\n' characters break lines (one stacked
// centred line each, 1.25x em advance) so multi-line subtitle bars render as
// authored; bold/italic/underline are synthesised from the one face. The
// title's own alpha and the clip's opacity dissolve over the existing pixels.
// The typeface comes from clip.title.font_family via find_font_path_for.
// No-op when the canvas dims are invalid, the text is empty, or no system font
// resolves.
void render_clip_title(const Clip& clip, std::vector<uint8_t>& rgba, int width,
                       int height, std::size_t stride);

// Same as render_clip_title but with an explicit font path (used by the tests
// to pin determinism; runtime rendering goes through find_font_path()).
void render_clip_title_with_font(const Clip& clip, const std::string& font_path,
                                 std::vector<uint8_t>& rgba, int width, int height,
                                 std::size_t stride);

}  // namespace canvas::core::title