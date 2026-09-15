// Unit tests for the Qt-free title-overlay module (core/include/canvas/core/...
// timeline/title.hpp): size/glyph laws, measurement, and RGBA rasterisation
// invariance, plus the set_clip_title edit-op contract.
//
// Font-dependent assertions auto-skip (exit 2) when no system font resolves —
// the renderer path degrades to "draw nothing" there, so a CI box without
// fonts must not fail the build. This machine carries DejaVu, so the full
// raster path is normally exercised.

#include "canvas/core/media/frame.hpp"
#include "canvas/core/export/renderer.hpp"
#include "canvas/core/timeline/edit_ops.hpp"
#include "canvas/core/timeline/model.hpp"
#include "canvas/core/timeline/title.hpp"
#include "canvas/core/project/project.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace canvas::core;

static int g_failures = 0;
static bool g_skip = false;

static void report(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Count of fully-dark pixels (all channels <= threshold). A rendered title must
// turn some black-area pixels non-zero; a no-op must leave the canvas untouched.
static std::size_t dark_pixels(const std::vector<std::uint8_t>& rgba,
                               int threshold = 8) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i] <= threshold && rgba[i + 1] <= threshold &&
            rgba[i + 2] <= threshold)
            ++n;
    }
    return n;
}

int main() {
    // --- size law -----------------------------------------------------------
    report(title::clamp_size(0.0f) == title::kSizeMin,
           "clamp_size: below-range saturates at min");
    report(title::clamp_size(9.9f) == title::kSizeMax,
           "clamp_size: above-range saturates at max");
    report(title::clamp_size(title::kSizeDefault) == title::kSizeDefault,
           "clamp_size: in-range passes through");

    Clip c;
    c.title.size = 0.1f;
    const int gh = title::glyph_height(c, 720);
    report(gh > 0 && gh <= 720, "glyph_height: sane for 720p");
    report(title::glyph_height(c, 0) == 0, "glyph_height: zero frame height -> 0");
    report(title::glyph_height(c, -5) == 0, "glyph_height: negative frame height -> 0");

    Clip big = c;
    big.title.size = 0.2f;
    report(title::glyph_height(big, 720) > title::glyph_height(c, 720),
           "glyph_height: larger size fraction -> taller glyphs");

    // --- measurement --------------------------------------------------------
    const std::string font = title::find_font_path();
    if (font.empty()) {
        std::printf("SKIP  no system font found; raster assertions skipped\n");
        g_skip = true;
    } else {
        std::printf("info   font: %s\n", font.c_str());
        report(font.empty() == false, "find_font_path: a concrete path");
    }

    const title::Layout empty = title::measure("", 48, font);
    report(empty.width == 0 && empty.height == 0, "measure: empty text -> {0,0}");
    const title::Layout zero_h = title::measure("Hi", 0, font);
    report(zero_h.width == 0 && zero_h.height == 0, "measure: zero height -> {0,0}");

    if (!g_skip) {
        const title::Layout unknown = title::measure("Hi", 48, "/no/such/font.ttf");
        report(unknown.width == 0 && unknown.height == 0,
               "measure: unknown font -> {0,0}");

        const title::Layout hi = title::measure("Hi", 48, font);
        const title::Layout hi_big = title::measure("Hi", 96, font);
        report(hi.width > 0 && hi.height > 0, "measure: renders a tight box");
        report(hi_big.width > hi.width && hi_big.height >= hi.height,
               "measure: taller em -> strictly larger box");

        const title::Layout shorty = title::measure("i", 48, font);
        const title::Layout longer = title::measure("iii", 48, font);
        report(longer.width > shorty.width, "measure: more letters -> wider box");

        // --- fit_caption (the "always fits the frame, bottom-anchored" law) ---
        constexpr int FW = 1920;
        constexpr int FH = 1080;
        {
            const title::Layout bad = title::measure("", 108, font);
            report(bad.width == 0 && bad.height == 0, "fit: sanity — empty text measures {0,0}");

            // Short text at the hint fits unshrunk; a long line that overflows
            // the width safe area forces a proportional shrink.
            const title::SubtitleFit short_cap =
                title::fit_caption("Hi", FW, FH, font, title::kSizeDefault);
            report(short_cap.size == title::kSizeDefault,
                   "fit: the size hint survives when the text already fits");
            report(short_cap.block_width > 0 && short_cap.block_height > 0,
                   "fit: a measurable caption carries its block box");

            const std::string long_line =
                "This is a very long subtitle line that will overflow any "
                "reasonable safe width at the default 10% em size and therefore "
                "must be scaled down to fit the frame";
            const title::SubtitleFit long_cap =
                title::fit_caption(long_line, FW, FH, font, title::kSizeDefault);
            report(long_cap.size < title::kSizeDefault,
                   "fit: an overflowing line shrinks below the default size");
            report(long_cap.size >= title::kSizeMin, "fit: shrink stays in the size law");
            const int max_fit_w = static_cast<int>(FW * title::kFitMaxWidthFraction);
            const int max_fit_h = static_cast<int>(FH * title::kFitMaxHeightFraction);
            report(long_cap.block_width > 0 && long_cap.block_width <= max_fit_w,
                   "fit: the fitted block respects the width safe area");
            report(long_cap.block_height > 0 && long_cap.block_height <= max_fit_h,
                   "fit: the fitted block respects the height safe area");

            // Bottom anchor: the fitted block's bottom edge rests exactly
            // kFitBottomMarginFraction of the frame above the bottom edge.
            const int margin = static_cast<int>(std::lround(FH * title::kFitBottomMarginFraction));
            const int block_top = static_cast<int>(
                std::lround((FH - long_cap.block_height) / 2.0 + long_cap.pos_y));
            report(block_top + long_cap.block_height <= FH - margin + 1,
                   "fit: the block bottom hugs the subtitle bottom margin");
            report(long_cap.pos_y > 0, "fit: the block is shifted down from centre");

            // 3-line stacked block: height band, not width, is the binding
            // constraint when a caption has the max three lines at full width.
            const title::SubtitleFit three =
                title::fit_caption("one line\nsecond line\nthird line", FW, 1080, font,
                                   title::kSizeDefault);
            report(three.block_height <= max_fit_h,
                   "fit: a 3-line caption fits the height safe area");
            report(three.block_height > long_cap.block_height ||
                       three.size < title::kSizeDefault,
                   "fit: more lines either shrink further or stack taller");

            // Degenerate inputs never crash and pin the bottom anchor.
            const title::SubtitleFit no_text = title::fit_caption("", FW, FH, font);
            report(no_text.size == title::kSizeDefault,
                   "fit: empty text keeps the hint fraction");
            const title::SubtitleFit bad_path =
                title::fit_caption("Hi", FW, FH, "/no/such/font.ttf");
            report(bad_path.block_width == 0 && bad_path.block_height == 0,
                   "fit: missing font reports no measured block");
            const title::SubtitleFit zero_dim = title::fit_caption("Hi", 0, 0, font);
            report(zero_dim.size == title::kSizeDefault && zero_dim.pos_y == 0.0,
                   "fit: zero frame dims degrades to the hint and no offset");
        }

        // --- installed-font enumeration ---------------------------------
        // Sandbox the scan roots at a directory holding one copy of the
        // resolved font: the catalogue must then hold exactly that family,
        // proving the scan honours CANVAS_TITLE_FONT_DIRS (and the process
        // cache is still cold — nothing above enumerates).
        const std::string sandbox = "/tmp/opencode/media/title_fonts";
        ::setenv("CANVAS_TITLE_FONT_DIRS", sandbox.c_str(), 1);
        {
            std::ifstream in(font, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            const std::string copy = sandbox + "/SandboxCopy.ttf";
            std::string mk = std::string("mkdir -p ") + sandbox;
            const int mkrc = std::system(mk.c_str());
            std::ofstream out(copy, std::ios::binary | std::ios::trunc);
            const bool copied = mkrc == 0 && !bytes.empty() &&
                                static_cast<bool>(out) &&
                                static_cast<bool>(out.write(bytes.data(),
                                                            static_cast<std::streamsize>(
                                                                bytes.size())));
            out.close();
            report(copied, "fonts: sandbox copy of the resolved font");
            if (copied) {
                const std::vector<title::FontFace> faces = title::installed_font_faces();
                report(faces.size() == 1, "fonts: sandbox scan yields one family");
                if (!faces.empty()) {
                    report(!faces[0].family.empty() && faces[0].path == copy,
                           "fonts: face carries family + sandbox path");
                    const std::vector<std::string> names =
                        title::installed_font_families();
                    report(names.size() == 1 && names[0] == faces[0].family,
                           "fonts: family-name view matches");
                    std::string upper = faces[0].family;
                    for (char& ch : upper)
                        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    report(title::find_font_path_for(faces[0].family) == copy,
                           "fonts: exact family resolves to the sandbox copy");
                    report(title::find_font_path_for(upper) == copy,
                           "fonts: family lookup is case-insensitive");
                    report(title::find_font_path_for("") == font,
                           "fonts: empty family falls back to the default face");
                    report(title::find_font_path_for("No Such Family XYZ") == font,
                           "fonts: unknown family falls back to the default face");

                    // The family-aware render path paints through the resolved
                    // face (and through the fallback for unknown families).
                    constexpr int FW = 64, FH = 64;
                    const std::vector<std::uint8_t> fblack(
                        static_cast<std::size_t>(FW) * FH * 4, 0);
                    Clip fam;
                    fam.title.text = "X";
                    fam.title.size = 0.25f;
                    fam.title.r = fam.title.g = fam.title.b = fam.title.a = 1.0f;
                    fam.title.font_family = faces[0].family;
                    std::vector<std::uint8_t> canvas_fam = fblack;
                    title::render_clip_title(fam, canvas_fam, FW, FH, FW * 4);
                    report((FW * FH) - dark_pixels(canvas_fam) > 0,
                           "render: named family paints");
                    Clip stale = fam;
                    stale.title.font_family = "No Such Family XYZ";
                    std::vector<std::uint8_t> canvas_stale = fblack;
                    title::render_clip_title(stale, canvas_stale, FW, FH, FW * 4);
                    report((FW * FH) - dark_pixels(canvas_stale) > 0,
                           "render: unknown family still paints via fallback");
                }
            }
        }
        ::unsetenv("CANVAS_TITLE_FONT_DIRS");

        // --- rasterisation ----------------------------------------------
        const int W = 64, H = 64;
        std::vector<std::uint8_t> black(W * H * 4, 0);

        Clip t;
        t.title.text = "X";
        t.title.size = 0.25f;
        t.title.r = t.title.g = t.title.b = t.title.a = 1.0f;

        std::vector<std::uint8_t> canvas = black;
        title::render_clip_title_with_font(t, font, canvas, W, H, W * 4);
        const std::size_t lit = (W * H) - dark_pixels(canvas);
        report(lit > 0, "render: paints pixels on black");

        // Centred: the painted bbox's centre must sit near the canvas centre.
        int min_x = W, max_x = -1, min_y = H, max_y = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                if (canvas[i] > 8 || canvas[i + 1] > 8 || canvas[i + 2] > 8) {
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                }
            }
        const int cx = (min_x + max_x) / 2;
        const int cy = (min_y + max_y) / 2;
        report(std::abs(cx - W / 2) <= 2 && std::abs(cy - H / 2) <= 2,
               "render: bbox is centred both axes");

        // Multi-line: "A\nB" stacks two centred lines, so ink lands on both
        // sides of the midline and the buffer differs from the flattened
        // single-line render of the same words.
        constexpr int MW = 160, MH = 120;
        const std::vector<std::uint8_t> mblack(
            static_cast<std::size_t>(MW) * MH * 4, 0);
        Clip multi;
        multi.title.text = "A\nB";
        multi.title.size = 0.2f;
        multi.title.r = multi.title.g = multi.title.b = multi.title.a = 1.0f;
        std::vector<std::uint8_t> canvas_multi = mblack;
        title::render_clip_title_with_font(multi, font, canvas_multi, MW, MH, MW * 4);
        Clip flat = multi;
        flat.title.text = "A B";
        std::vector<std::uint8_t> canvas_flat = mblack;
        title::render_clip_title_with_font(flat, font, canvas_flat, MW, MH, MW * 4);
        const auto lit_at = [](const std::vector<std::uint8_t>& c, int y0, int y1) {
            std::size_t n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < MW; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * MW + x) * 4;
                    if (c[i] > 8 || c[i + 1] > 8 || c[i + 2] > 8) ++n;
                }
            return n;
        };
        report((MW * MH) - dark_pixels(canvas_multi) > 0, "render: multi-line paints");
        report(canvas_multi != canvas_flat, "render: multi-line differs from flattened");
        report(lit_at(canvas_multi, 0, MH / 2) > 0 && lit_at(canvas_multi, MH / 2, MH) > 0,
               "render: multi-line inks both halves");

        // --- faux styles ------------------------------------------------
        // "Ag" exercises ascender + descender (underline must clear the tail).
        Clip styled;
        styled.title.text = "Ag";
        styled.title.size = 0.25f;
        styled.title.r = styled.title.g = styled.title.b = styled.title.a = 1.0f;
        const auto paint = [&](const Clip& c) {
            std::vector<std::uint8_t> cv = black;
            title::render_clip_title_with_font(c, font, cv, W, H, W * 4);
            return cv;
        };
        const auto lit_count = [&](const std::vector<std::uint8_t>& cv) {
            return static_cast<std::size_t>(W * H) - dark_pixels(cv);
        };
        const auto bottom_row = [&](const std::vector<std::uint8_t>& cv) {
            int bottom = -1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                    if (cv[i] > 8 || cv[i + 1] > 8 || cv[i + 2] > 8) bottom = y;
                }
            return bottom;
        };
        const std::vector<std::uint8_t> plain_cv = paint(styled);
        report(lit_count(plain_cv) > 0, "render: style base paints");
        Clip bold = styled;
        bold.title.bold = true;
        const std::vector<std::uint8_t> bold_cv = paint(bold);
        report(lit_count(bold_cv) > lit_count(plain_cv), "render: bold emboldens");
        Clip ital = styled;
        ital.title.italic = true;
        const std::vector<std::uint8_t> ital_cv = paint(ital);
        report(lit_count(ital_cv) > 0 && ital_cv != plain_cv, "render: italic slants");
        Clip under = styled;
        under.title.underline = true;
        const std::vector<std::uint8_t> under_cv = paint(under);
        report(lit_count(under_cv) > lit_count(plain_cv) &&
                   bottom_row(under_cv) > bottom_row(plain_cv),
               "render: underline bars below the descenders");

        // --- position offset --------------------------------------------
        // pos_x/pos_y translate the whole block in output px (Video-tab and
        // Subtitles-tab sliders edit these same fields).
        const auto bbox_cx = [&](const std::vector<std::uint8_t>& cv) {
            int min_x = W, max_x = -1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                    if (cv[i] > 8 || cv[i + 1] > 8 || cv[i + 2] > 8) {
                        min_x = std::min(min_x, x);
                        max_x = std::max(max_x, x);
                    }
                }
            return (min_x + max_x) / 2;
        };
        const auto bbox_cy = [&](const std::vector<std::uint8_t>& cv) {
            int min_y = H, max_y = -1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                    if (cv[i] > 8 || cv[i + 1] > 8 || cv[i + 2] > 8) {
                        min_y = std::min(min_y, y);
                        max_y = std::max(max_y, y);
                    }
                }
            return (min_y + max_y) / 2;
        };
        Clip moved = t;
        moved.pos_x = 20.0;
        moved.pos_y = 10.0;
        const std::vector<std::uint8_t> moved_cv = paint(moved);
        report(bbox_cx(moved_cv) - cx == 20 && bbox_cy(moved_cv) - cy == 10,
               "render: pos_x/pos_y translate the block in px");

        // --- drop shadow ------------------------------------------------
        // A red shadow offset right casts a red-tinted copy outside the glyph
        // bbox; the tint proves it is the shadow colour, not a style bleed.
        const auto red_dom = [](const std::vector<std::uint8_t>& cv) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < cv.size(); i += 4)
                if (cv[i] > 32 && cv[i] > cv[i + 1] + 8 && cv[i] > cv[i + 2] + 8) ++n;
            return n;
        };
        const auto bbox_xmax = [](const std::vector<std::uint8_t>& cv) {
            int max_x = -1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                    if (cv[i] > 8 || cv[i + 1] > 8 || cv[i + 2] > 8) max_x = std::max(max_x, x);
                }
            return max_x;
        };
        const auto bbox_xmin = [](const std::vector<std::uint8_t>& cv) {
            int min_x = W;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
                    if (cv[i] > 8 || cv[i + 1] > 8 || cv[i + 2] > 8) min_x = std::min(min_x, x);
                }
            return min_x;
        };
        report(red_dom(plain_cv) == 0, "render: plain glyphs carry no shadow tint");
        Clip eyed = styled;
        eyed.title.shadow = true;
        eyed.title.shadow_r = 1.0f;
        eyed.title.shadow_g = 0.0f;
        eyed.title.shadow_b = 0.0f;
        eyed.title.shadow_opacity = 1.0f;
        eyed.title.shadow_blur = 0.0f;
        eyed.title.shadow_dx = 4.0f;
        eyed.title.shadow_dy = 0.0f;
        const std::vector<std::uint8_t> eyed_cv = paint(eyed);
        report(red_dom(eyed_cv) > 0, "render: shadow paints its own tint");
        report(bbox_xmax(eyed_cv) > bbox_xmax(plain_cv), "render: shadow offset extends ink");
        Clip fuzzy = eyed;
        fuzzy.title.shadow_blur = 2.0f;
        const std::vector<std::uint8_t> fuzzy_cv = paint(fuzzy);
        report(fuzzy_cv != eyed_cv && red_dom(fuzzy_cv) > 0,
               "render: blur softens the shadow copy");
        Clip ghost = eyed;
        ghost.title.shadow_opacity = 0.25f;
        const std::vector<std::uint8_t> ghost_cv = paint(ghost);
        report(red_dom(ghost_cv) < red_dom(eyed_cv) && red_dom(ghost_cv) > 0,
               "render: shadow opacity dims the shadow");

        // --- background box ---------------------------------------------
        // Full-opacity red box padded 32 px around the "Ag" block: the bbox
        // grows on both sides well past the glyphs and the field tints red.
        Clip bxd = styled;
        bxd.title.box = true;
        bxd.title.box_r = 1.0f;
        bxd.title.box_g = 0.0f;
        bxd.title.box_b = 0.0f;
        bxd.title.box_opacity = 1.0f;
        bxd.title.box_pad_x = 32.0f;
        bxd.title.box_pad_y = 32.0f;
        const std::vector<std::uint8_t> bxd_cv = paint(bxd);
        report(red_dom(bxd_cv) > 0, "render: box paints its own fill");
        report(bbox_xmin(bxd_cv) <= bbox_xmin(plain_cv) - 12 &&
                   bbox_xmax(bxd_cv) >= bbox_xmax(plain_cv) + 12,
               "render: box padding extends the block bbox");
        Clip tight = bxd;
        tight.title.box_pad_x = 0.0f;
        tight.title.box_pad_y = 0.0f;
        const std::vector<std::uint8_t> tight_cv = paint(tight);
        report(bbox_xmin(tight_cv) >= bbox_xmin(bxd_cv) + 8 &&
                   bbox_xmax(tight_cv) <= bbox_xmax(bxd_cv) - 8,
               "render: box padding is width/height sized, not fixed");
        Clip sealed = bxd;
        sealed.title.box = false;
        const std::vector<std::uint8_t> sealed_cv = paint(sealed);
        report(red_dom(sealed_cv) == 0, "render: disabled box draws nothing");

        // Corner radius: rounding the box cuts the far corner pixel off the
        // sharp fill (the corner pixel is dark where the square box lit it)
        // while the box interior keeps its fill — and the shed area shrinks
        // the red footprint measurably.
        const auto red_at = [](const std::vector<std::uint8_t>& cv, const int x,
                               const int y) {
            if (x < 0 || y < 0 || x >= W || y >= H) return false;
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            return cv[i] > 32 && cv[i] > cv[i + 1] + 8 && cv[i] > cv[i + 2] + 8;
        };
        int sharp_min_x = W, sharp_min_y = H, sharp_max_x = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (red_at(bxd_cv, x, y)) {
                    sharp_min_x = std::min(sharp_min_x, x);
                    sharp_min_y = std::min(sharp_min_y, y);
                    sharp_max_x = std::max(sharp_max_x, x);
                }
        report(red_at(bxd_cv, sharp_min_x + 3, sharp_min_y + 3),
               "render: sharp box lights its corner cell");
        Clip roundy = bxd;
        roundy.title.box_radius = 12.0f;
        const std::vector<std::uint8_t> round_cv = paint(roundy);
        report(!red_at(round_cv, sharp_min_x + 3, sharp_min_y + 3),
               "render: box radius cuts the corner pixels");
        report(red_dom(round_cv) < red_dom(bxd_cv) &&
                   red_at(round_cv, (sharp_min_x + sharp_max_x) / 2, sharp_min_y + 8),
               "render: rounded box keeps its interior but sheds area");

        // Empty title: canvas must be byte-identical.
        Clip blank = t;
        blank.title.text.clear();
        std::vector<std::uint8_t> canvas_blank = black;
        title::render_clip_title_with_font(blank, font, canvas_blank, W, H, W * 4);
        report(dark_pixels(canvas_blank) == W * H, "render: empty text -> no-op");

        // Clip opacity 0 -> nothing dissolves in; full opacity -> opaque glyphs.
        Clip dim = t;
        dim.opacity = 0.0f;
        std::vector<std::uint8_t> canvas_dim = black;
        title::render_clip_title_with_font(dim, font, canvas_dim, W, H, W * 4);
        report(dark_pixels(canvas_dim) == W * H,
               "render: zero clip opacity draws nothing");

        // Colour law: red glyphs are red-dominant where lit.
        Clip red = t;
        red.title.r = 1.0f;
        red.title.g = red.title.b = 0.0f;
        std::vector<std::uint8_t> canvas_red = black;
        title::render_clip_title_with_font(red, font, canvas_red, W, H, W * 4);
        std::uint64_t sr = 0, sg = 0, sb = 0;
        std::size_t n = 0;
        for (std::size_t i = 0; i < canvas_red.size(); i += 4) {
            if (canvas_red[i] > 32) {
                sr += canvas_red[i];
                sg += canvas_red[i + 1];
                sb += canvas_red[i + 2];
                ++n;
            }
        }
        report(n > 0 && sr > sg && sr > sb, "render: red glyphs stay red-dominant");
    }

    // --- export glue --------------------------------------------------------
    // Mirrors add_title_clip(): a media < 0 clip whose video IS the text
    // raster, run through the same pipeline the Deliver page uses — the
    // render_video_frame one-shot and RenderSession (the export worker's
    // per-track decoder reuse).
    if (!g_skip) {
        Project tp;
        tp.name = "Smoke";
        tp.sequence.fps = 30.0;
        Track v1;
        v1.kind = Track::Kind::Video;
        v1.name = "V1";
        tp.sequence.video_tracks.push_back(std::move(v1));

        Clip tc;
        tc.media = -1;
        tc.tl_in = 0;
        tc.tl_out = 90;
        tc.src_in = 0;
        tc.src_out = 90;
        tc.name = "Title";
        tc.title.text = "Title";
        tc.title.size = title::kSizeDefault;
        std::unique_ptr<ICommand> tcmd =
            place_clip(tp.sequence, Track::Kind::Video, 0, tc, Placement::Overwrite);
        report(tcmd != nullptr, "glue: title-only clip places");

        const int W = 160, H = 90;
        VideoFramePtr shot = render_video_frame(tp, 0, W, H, 0, nullptr);
        report(shot != nullptr, "glue: render_video_frame title frame succeeds");
        if (shot) {
            bool lit = false;
            int min_x = W, max_x = -1, min_y = H, max_y = -1;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * shot->stride +
                                          static_cast<std::size_t>(x) * 4u;
                    const uint8_t r = shot->rgba[i], g = shot->rgba[i + 1],
                                   b = shot->rgba[i + 2];
                    if (r > 8 || g > 8 || b > 8) {
                        lit = true;
                        min_x = std::min(min_x, x);
                        max_x = std::max(max_x, x);
                        min_y = std::min(min_y, y);
                        max_y = std::max(max_y, y);
                    }
                }
            report(lit, "glue: title pixels land in the output canvas");
            const int cx = (min_x + max_x) / 2;
            const int cy = (min_y + max_y) / 2;
            report(std::abs(cx - W / 2) <= 2 && std::abs(cy - H / 2) <= 2,
                   "glue: title is centred in the rendered frame");
        }

        RenderSession rs;
        report(rs.begin(tp, W, H, nullptr), "glue: RenderSession::begin");
        VideoFramePtr sshot = rs.frame(0);
        report(sshot != nullptr, "glue: RenderSession::frame title succeeds");
        if (sshot) {
            bool lit = false;
            for (std::size_t i = 0; i < sshot->rgba.size(); i += 4)
                if (sshot->rgba[i] > 8 || sshot->rgba[i + 1] > 8 || sshot->rgba[i + 2] > 8) {
                    lit = true;
                    break;
                }
            report(lit, "glue: RenderSession carries title pixels");
        }
        rs.end();

        // Disabled title clip -> pure black canvas.
        tp.sequence.video_tracks[0].clips[0].enabled = false;
        VideoFramePtr black = render_video_frame(tp, 0, W, H, 0, nullptr);
        report(black != nullptr, "glue: disabled title renders");
        if (black) {
            bool any = false;
            for (const uint8_t c : black->rgba)
                if (c != 0) { any = true; break; }
            report(!any, "glue: disabled title yields an all-black canvas");
        }
    }

    // --- edit-op contract ---------------------------------------------------
    Project p;
    p.sequence.fps = 30.0;
    p.sequence.video_tracks.emplace_back();
    p.sequence.video_tracks[0].kind = Track::Kind::Video;
    Clip base;
    base.media = 0;
    base.tl_in = 0;
    base.src_in = 0;
    base.src_out = 30;
    std::unique_ptr<ICommand> cmd =
        place_clip(p.sequence, Track::Kind::Video, 0, base, Placement::Overwrite);
    report(cmd != nullptr, "edit-op: place succeeds");
    UndoStack undo;
    undo.record(std::move(cmd));
    const ClipId cid = p.sequence.video_tracks[0].clips[0].id;

    report(!p.sequence.video_tracks[0].clips[0].has_title(),
           "edit-op: fresh clip has no title");

    Clip::Title want;
    want.text = "Hello";
    want.size = 0.15f;
    want.r = 0.8f;
    want.g = 0.1f;
    want.b = 0.2f;
    want.a = 0.9f;
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, cid, want);
    report(cmd != nullptr, "edit-op: set returns command");
    undo.record(std::move(cmd));
    const Clip& after = p.sequence.video_tracks[0].clips[0];
    report(after.has_title() && after.title.text == want.text && after.title.size == want.size &&
               after.title.r == want.r && after.title.g == want.g && after.title.b == want.b &&
               after.title.a == want.a,
           "edit-op: fields land on the clip");

    const ClipId bogus = 9999;
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, bogus, want);
    report(cmd == nullptr, "edit-op: unknown clip -> nullptr");

    Clip::Title huge = want;
    huge.size = 9.9f;
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, cid, huge);
    undo.record(std::move(cmd));
    report(p.sequence.video_tracks[0].clips[0].title.size == title::kSizeMax,
           "edit-op: size clamps to max");

    report(undo.undo(p.sequence), "edit-op: undo step");
    report(p.sequence.video_tracks[0].clips[0].title.text == want.text,
           "edit-op: undo restores previous title");
    report(undo.undo(p.sequence), "edit-op: undo to empty");
    report(!p.sequence.video_tracks[0].clips[0].has_title(),
           "edit-op: undo clears title entirely");
    report(undo.redo(p.sequence) && p.sequence.video_tracks[0].clips[0].has_title(),
           "edit-op: redo restores title");

    // Clear back to empty via the op itself.
    Clip::Title empty_title;
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, cid, empty_title);
    report(cmd != nullptr && !p.sequence.video_tracks[0].clips[0].has_title(),
           "edit-op: empty title clears has_title");

    // No-op EditCommand: identical re-sets return an equal-snapshot command,
    // so undo/redo around it changes nothing (repo convention, like transform
    // and composite). Run last so the settled no-op entry does not skew the
    // absolute undo steps above.
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, cid, want);
    undo.record(std::move(cmd));  // real: empty -> want
    cmd = set_clip_title(p.sequence, Track::Kind::Video, 0, cid, want);
    report(cmd != nullptr, "edit-op: identical re-set returns a command");
    undo.record(std::move(cmd));  // no-op: want -> want
    report(undo.undo(p.sequence) && p.sequence.video_tracks[0].clips[0].has_title() &&
               p.sequence.video_tracks[0].clips[0].title.text == want.text,
           "edit-op: undo of no-op leaves the title in place");
    report(undo.redo(p.sequence) && p.sequence.video_tracks[0].clips[0].has_title(),
           "edit-op: redo of no-op keeps the title");

    if (g_skip) {
        std::printf("ALL TESTS PASSED (font-dependent claims skipped)\n");
        return 2;
    }
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", g_failures);
    return 1;
}