#include "canvas/core/timeline/title.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>

// Vendored public-domain TrueType rasteriser (keeping stb_truetype itself out
// of the public include surface is a compile-time guard: its own license header
// lives at the top of the vendored file, which one dirt-cheap copy keeps us on
// the no-security-guarantee contract). title.cpp is the only TU that includes
// it; the STB_TRUETYPE_IMPLEMENTATION macro compiles the implementation here.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace canvas::core::title {

float clamp_size(const float size) noexcept {
    return std::clamp(size, kSizeMin, kSizeMax);
}

namespace {

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// UTF-8 -> codepoint run. Invalid sequences emit U+FFFD; control characters
// (tabs, carriage returns, ...) collapse to a space. Newlines never reach here
// — split_lines() breaks the text into lines first, so one codepoint run is
// always one centred line. Stops at a NUL.
std::vector<uint32_t> codepoints(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        uint32_t cp = 0;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        if (i + len > n) {
            out.push_back(0xFFFD);
            break;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const uint8_t cc = static_cast<uint8_t>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                cp = 0xFFFD;
                len = 1;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += len;
        if (cp < 0x20 || cp == 0x7F) cp = ' ';
        out.push_back(cp);
    }
    return out;
}

struct LoadedFont {
    std::vector<uint8_t> data;
    stbtt_fontinfo info {};
    bool ok = false;
};

std::shared_ptr<const LoadedFont> load_font(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<LoadedFont>> cache;
    std::lock_guard<std::mutex> lock(mu);
    const auto it = cache.find(path);
    if (it != cache.end()) return it->second;

    auto font = std::make_shared<LoadedFont>();
    if (FILE* fp = std::fopen(path.c_str(), "rb")) {
        std::fseek(fp, 0, SEEK_END);
        const long len = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (len > 0) {
            font->data.resize(static_cast<std::size_t>(len));
            const std::size_t got =
                std::fread(font->data.data(), 1, font->data.size(), fp);
            font->ok = got == font->data.size() &&
                       stbtt_InitFont(&font->info, font->data.data(), 0) != 0;
        }
        std::fclose(fp);
    }
    cache[path] = font;
    return font;
}

// Tight pixel box of the whole line: width/height plus the origin of the first
// bitmap pixel relative to the pen start (origin_x) and the boxes' top
// (origin_y). pen tracks cumulative advance + kerning in pixels.
struct GlyphLayout {
    int width = 0;
    int height = 0;
    int origin_x = 0;
    int origin_y = 0;
};

GlyphLayout layout_line(const LoadedFont& font, const std::vector<uint32_t>& cps,
                        float scale) {
    GlyphLayout out;
    double pen = 0.0;
    double min_x = 0.0, max_x = 0.0, min_y = 0.0, max_y = 0.0;
    bool first = true;
    int prev_glyph = 0;
    for (const uint32_t cp : cps) {
        int advance = 0;
        const int glyph = stbtt_FindGlyphIndex(&font.info, static_cast<int>(cp));
        if (glyph == 0) continue;
        stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, nullptr);
        if (prev_glyph != 0)
            pen += stbtt_GetGlyphKernAdvance(&font.info, prev_glyph, glyph) * scale;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&font.info, glyph, scale, scale, &x0, &y0, &x1, &y1);
        const double px0 = pen + static_cast<double>(x0);
        const double px1 = pen + static_cast<double>(x1);
        if (first) {
            min_x = px0;
            max_x = px1;
            min_y = static_cast<double>(y0);
            max_y = static_cast<double>(y1);
            first = false;
        } else {
            min_x = std::min(min_x, px0);
            max_x = std::max(max_x, px1);
            min_y = std::min(min_y, static_cast<double>(y0));
            max_y = std::max(max_y, static_cast<double>(y1));
        }
        pen += static_cast<double>(advance) * scale;
        prev_glyph = glyph;
    }
    if (first) return out;
    out.width = std::max(1, static_cast<int>(std::ceil(max_x - min_x)));
    out.height = std::max(1, static_cast<int>(std::ceil(max_y - min_y)));
    out.origin_x = static_cast<int>(std::lround(min_x));
    out.origin_y = static_cast<int>(std::lround(min_y));
    return out;
}

// --- installed-font enumeration ----------------------------------------------
// Qt-free system font discovery for the Inspector's font dropdown: scan the
// font roots for .ttf/.otf files and read each file's name table with
// stb_truetype. Files are read standalone (never through the render cache) so
// enumeration doesn't pin every system font in memory.

std::string ascii_lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim_ws(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// UTF-16BE (BMP) -> UTF-8; lone surrogates become U+FFFD.
std::string utf16be_to_utf8(const char* data, const int len) {
    std::string out;
    out.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i + 1 < len; i += 2) {
        uint32_t cp = (static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 8) |
                      static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1]));
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Best-effort name-table read: MS Unicode BMP English first, then the Unicode
// platform BMP entry, then Mac Roman (ASCII-subset decode).
std::string font_name_string(const stbtt_fontinfo& info, const int name_id) {
    int len = 0;
    const char* s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT,
                                            STBTT_MS_EID_UNICODE_BMP,
                                            STBTT_MS_LANG_ENGLISH, name_id);
    if (s && len > 0) return trim_ws(utf16be_to_utf8(s, len));
    s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_UNICODE,
                                STBTT_UNICODE_EID_UNICODE_2_0_BMP, 0, name_id);
    if (s && len > 0) return trim_ws(utf16be_to_utf8(s, len));
    s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
                                STBTT_MAC_LANG_ENGLISH, name_id);
    if (s && len > 0) {
        std::string out;
        for (int i = 0; i < len; ++i) {
            const unsigned char ch = static_cast<unsigned char>(s[i]);
            out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        return trim_ws(out);
    }
    return {};
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out,
                     const std::size_t cap) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    bool ok = false;
    if (len > 0 && static_cast<std::size_t>(len) <= cap) {
        out.resize(static_cast<std::size_t>(len));
        ok = std::fread(out.data(), 1, out.size(), fp) == out.size();
    }
    std::fclose(fp);
    return ok;
}

struct FaceNames {
    std::string family;
    std::string style;
};

FaceNames read_face_names(const std::string& path) {
    FaceNames out;
    std::vector<uint8_t> data;
    if (!read_file_bytes(path, data, 64u * 1024u * 1024u)) return out;
    stbtt_fontinfo info;
    if (stbtt_InitFont(&info, data.data(), 0) == 0) return out;
    // Typographic names (16/17) first — they carry the real family for faces
    // whose legacy family field is overloaded (e.g. "... Semibold").
    out.family = font_name_string(info, 16);
    if (out.family.empty()) out.family = font_name_string(info, 1);
    out.style = font_name_string(info, 17);
    if (out.style.empty()) out.style = font_name_string(info, 2);
    return out;
}

std::vector<std::string> font_scan_roots() {
    if (const char* e = std::getenv("CANVAS_TITLE_FONT_DIRS"); e && *e) {
        std::vector<std::string> roots;
        std::string cur;
        for (const char* p = e;; ++p) {
            if (*p == ':' || *p == '\0') {
                if (!cur.empty()) roots.push_back(cur);
                cur.clear();
                if (*p == '\0') break;
            } else {
                cur.push_back(*p);
            }
        }
        return roots;
    }
    std::vector<std::string> roots{"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* h = std::getenv("HOME"); h && *h) {
        roots.emplace_back(std::string(h) + "/.fonts");
        roots.emplace_back(std::string(h) + "/.local/share/fonts");
    }
    return roots;
}

bool has_font_suffix(const std::string& name) {
    const std::size_t pos = name.rfind('.');
    if (pos == std::string::npos) return false;
    const std::string ext = ascii_lower(name.substr(pos));
    return ext == ".ttf" || ext == ".otf";
}

void scan_fonts_dir(const std::string& dir, std::vector<std::string>& out, const int depth) {
    if (depth > 8) return;  // symlink-loop guard; real font trees are shallow
    DIR* dp = ::opendir(dir.c_str());
    if (!dp) return;
    while (dirent* ent = ::readdir(dp)) {
        const std::string name = ent->d_name ? ent->d_name : "";
        if (name.empty() || name[0] == '.') continue;
        const std::string full = dir + "/" + name;
        bool is_dir = ent->d_type == DT_DIR;
        if (ent->d_type == DT_UNKNOWN || ent->d_type == DT_LNK) {
            struct stat st {};
            if (::stat(full.c_str(), &st) == 0) is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            scan_fonts_dir(full, out, depth + 1);
        } else if (has_font_suffix(name)) {
            out.push_back(full);
        }
    }
    ::closedir(dp);
}

std::vector<FontFace> scan_font_faces() {
    std::vector<std::string> files;
    for (const std::string& root : font_scan_roots()) scan_fonts_dir(root, files, 0);
    std::sort(files.begin(), files.end());
    // One face per family (keyed case-insensitively): the Regular style wins,
    // then the lexicographically smallest path — deterministic for a set.
    std::map<std::string, FontFace> best;
    std::map<std::string, std::string> best_style;
    for (const std::string& path : files) {
        FaceNames names = read_face_names(path);
        if (names.family.empty()) {
            // No parsable name table: fall back to the stem so the file is
            // still selectable rather than silently dropped.
            const std::size_t slash = path.rfind('/');
            std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
            const std::size_t dot = stem.rfind('.');
            if (dot != std::string::npos) stem.resize(dot);
            for (char& ch : stem)
                if (ch == '_' || ch == '-') ch = ' ';
            names.family = trim_ws(stem);
            if (names.family.empty()) continue;
        }
        const std::string key = ascii_lower(names.family);
        const bool regular = ascii_lower(names.style) == "regular";
        const auto it = best.find(key);
        if (it == best.end()) {
            best[key] = FontFace{names.family, path};
            best_style[key] = names.style;
        } else if (regular && ascii_lower(best_style[key]) != "regular") {
            it->second = FontFace{names.family, path};
            best_style[key] = names.style;
        }
    }
    std::vector<FontFace> out;
    out.reserve(best.size());
    for (auto& [key, face] : best) {
        (void)key;
        out.push_back(std::move(face));
    }
    std::sort(out.begin(), out.end(), [](const FontFace& a, const FontFace& b) {
        return ascii_lower(a.family) < ascii_lower(b.family);
    });
    return out;
}

// --- multi-line raster pieces ------------------------------------------------

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    lines.push_back(cur);
    return lines;
}

struct LineBitmap {
    std::vector<uint8_t> alpha;
    int w = 0;
    int h = 0;
    int bottom = 0;  // lowest glyph-bottom row in box coords (underline anchor)
};

// One rasterised line at its canvas origin (box + shadow + box extents all
// derive from this, so single- and multi-line share one blit tail).
struct Placed {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    const LineBitmap* bmp = nullptr;
};

// Faux styles synthesised from the one installed face: embolden overdraw,
// whole-box slant with a pinned baseline, underline bar below the descenders.
struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

// Rasterises one codepoint run into a tight alpha box (same advance/kerning/
// box math the single-line renderer always used — pixel-identical output when
// the style is plain).
LineBitmap rasterize_line(const LoadedFont& font, const std::vector<uint32_t>& cps,
                          const float scale, const TextStyle& style, const float em_px) {
    LineBitmap out;
    const GlyphLayout lay = layout_line(font, cps, static_cast<double>(scale));
    if (lay.width <= 0 || lay.height <= 0) return out;
    out.w = lay.width;
    out.h = lay.height;
    out.alpha.assign(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h), 0);

    double pen = 0.0;
    int prev_glyph = 0;
    int bottom = 0;
    for (const uint32_t cp : cps) {
        int advance = 0;
        const int glyph = stbtt_FindGlyphIndex(&font.info, static_cast<int>(cp));
        if (glyph == 0) continue;
        stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, nullptr);
        if (prev_glyph != 0)
            pen += stbtt_GetGlyphKernAdvance(&font.info, prev_glyph, glyph) * scale;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&font.info, glyph, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0;
        const int gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            const int gx =
                static_cast<int>(std::lround(pen + static_cast<double>(x0))) - lay.origin_x;
            const int gy = y0 - lay.origin_y;
            if (gx >= 0 && gy >= 0 && gx + gw <= out.w && gy + gh <= out.h) {
                stbtt_MakeGlyphBitmap(&font.info,
                                      &out.alpha[static_cast<std::size_t>(gy) * out.w + gx],
                                      gw, gh, out.w, scale, scale, glyph);
                bottom = std::max(bottom, gy + gh);
            }
        }
        pen += static_cast<double>(advance) * scale;
        prev_glyph = glyph;
    }

    // Faux bold: OR a right-shifted copy (embolden ~4% of em, min 1 px).
    if (style.bold) {
        const int dx = std::max(1, static_cast<int>(std::lround(em_px * 0.04f)));
        const int nw = out.w + dx;
        std::vector<uint8_t> widened(static_cast<std::size_t>(nw) * out.h, 0);
        for (int y = 0; y < out.h; ++y) {
            for (int x = 0; x < nw; ++x) {
                uint8_t v = 0;
                if (x < out.w)
                    v = std::max(v, out.alpha[static_cast<std::size_t>(y) * out.w + x]);
                if (x - dx >= 0 && x - dx < out.w)
                    v = std::max(
                        v, out.alpha[static_cast<std::size_t>(y) * out.w + x - dx]);
                widened[static_cast<std::size_t>(y) * nw + x] = v;
            }
        }
        out.alpha = std::move(widened);
        out.w = nw;
    }

    // Faux italic: whole-box shear (~0.2, about 11 degrees) with the baseline
    // pinned, padded both sides so slanted rows never clip.
    if (style.italic) {
        constexpr float kSlant = 0.2f;
        // Clamped: an all-descender line would otherwise pin the baseline
        // outside the box and shrink the padding negative.
        const int baseline = std::clamp(-lay.origin_y, 0, out.h);
        const int left_pad =
            static_cast<int>(std::ceil(kSlant * static_cast<float>(out.h - baseline)));
        const int right_pad =
            static_cast<int>(std::ceil(kSlant * static_cast<float>(baseline)));
        const int nw = out.w + left_pad + right_pad;
        std::vector<uint8_t> sheared(static_cast<std::size_t>(nw) * out.h, 0);
        for (int y = 0; y < out.h; ++y) {
            const int shift =
                static_cast<int>(std::lround(kSlant * static_cast<float>(baseline - y)));
            for (int x = 0; x < out.w; ++x) {
                const uint8_t v =
                    out.alpha[static_cast<std::size_t>(y) * out.w + x];
                if (v == 0) continue;
                const int dx = x + left_pad + shift;
                uint8_t& dst =
                    sheared[static_cast<std::size_t>(y) * nw + dx];
                dst = std::max(dst, v);
            }
        }
        out.alpha = std::move(sheared);
        out.w = nw;
    }
    out.bottom = bottom;

    // Underline: solid bar across the full line width below the descenders.
    if (style.underline) {
        const int thick = std::max(1, static_cast<int>(std::lround(em_px / 14.0f)));
        const int gap = std::max(1, static_cast<int>(std::lround(em_px * 0.08f)));
        const int y0 = bottom + gap;
        const int need = y0 + thick;
        if (need > out.h) {
            out.alpha.resize(static_cast<std::size_t>(out.w) * need, 0);
            out.h = need;
        }
        for (int y = y0; y < y0 + thick; ++y)
            for (int x = 0; x < out.w; ++x)
                out.alpha[static_cast<std::size_t>(y) * out.w + x] = 255;
    }
    return out;
}

// Alpha-over dissolve of one box onto the canvas (the shared composite tail).
void blit_alpha(const uint8_t* alpha, const int bw, const int bh, const int ox, const int oy,
                std::vector<uint8_t>& rgba, const int width, const int height,
                const std::size_t stride, const int cr, const int cg, const int cb,
                const float text_a, const float opacity) {
    for (int py = 0; py < bh; ++py) {
        const int cy = oy + py;
        if (cy < 0 || cy >= height) continue;
        uint8_t* row = rgba.data() + static_cast<std::size_t>(cy) * stride;
        const uint8_t* srow = alpha + static_cast<std::size_t>(py) * bw;
        for (int px = 0; px < bw; ++px) {
            const uint8_t ga = srow[px];
            if (ga == 0) continue;
            const int cx = ox + px;
            if (cx < 0 || cx >= width) continue;
            const float src_a = (static_cast<float>(ga) / 255.0f) * text_a * opacity;
            const float inv = 1.0f - src_a;
            uint8_t* dst = row + static_cast<std::size_t>(cx) * 4u;
            dst[0] = static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(static_cast<float>(cr) * src_a +
                                             static_cast<float>(dst[0]) * inv)),
                0, 255));
            dst[1] = static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(static_cast<float>(cg) * src_a +
                                             static_cast<float>(dst[1]) * inv)),
                0, 255));
            dst[2] = static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(static_cast<float>(cb) * src_a +
                                             static_cast<float>(dst[2]) * inv)),
                0, 255));
        }
    }
}

// Shared placement law for a block of rasterised lines (single-line centred
// box, or stacked centred lines with a 0.25 em gap), shifted by the clip's
// visual offset. Both the canvas renderer and the tight-sprite rasteriser use
// this one copy so their origins can never drift apart.
std::vector<Placed> place_block(const std::vector<LineBitmap>& bits, const int width,
                                const int height, const int off_x, const int off_y,
                                const float em_px) {
    if (bits.empty()) return {};
    std::vector<Placed> placed;
    placed.reserve(bits.size());
    if (bits.size() <= 1) {
        // Single line: the historical centred-box path, unchanged.
        const LineBitmap& b = bits.front();
        if (b.w > 0 && b.h > 0)
            placed.push_back(
                Placed{(width - b.w) / 2 + off_x, (height - b.h) / 2 + off_y, b.w, b.h, &b});
    } else {
        // Multi-line: stack the centred lines with a 0.25 em gap, the whole
        // block centred vertically. Empty lines keep their slot (blank band).
        const int gap = std::max(0, static_cast<int>(std::lround(em_px * 0.25f)));
        int total = 0;
        for (const LineBitmap& b : bits) total += b.h;
        total += gap * (static_cast<int>(bits.size()) - 1);
        int y = (height - total) / 2 + off_y;
        for (const LineBitmap& b : bits) {
            if (b.w > 0 && b.h > 0)
                placed.push_back(Placed{(width - b.w) / 2 + off_x, y, b.w, b.h, &b});
            y += b.h + gap;
        }
    }
    return placed;
}

// Alpha-over of one component into a full-canvas premultiplied-RGBA8
// accumulator (byte 3 = coverage), the sprite analogue of blit_alpha /
// fill_rounded_rect: premultiplied add `c*a` and coverage add `a*(1-A)`. The
// per-step rounding mirrors the CPU blob-per-component rounding, so the
// sprite's accumulated premultiplied colour matches the canvas composite's
// per-component law over an opaque background (up to one final blend pass).
void sprite_blend(uint8_t* rgba, const std::size_t stride, const int width, const int height,
                  const int x, const int y, const float r, const float g, const float b,
                  const float a) {
    if (a <= 0.0f || x < 0 || x >= width || y < 0 || y >= height) return;
    uint8_t* dst = rgba + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    const float inv = 1.0f - a;
    dst[0] = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(r * a + static_cast<float>(dst[0]) * inv)), 0, 255));
    dst[1] = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(g * a + static_cast<float>(dst[1]) * inv)), 0, 255));
    dst[2] = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(b * a + static_cast<float>(dst[2]) * inv)), 0, 255));
    dst[3] = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(a * 255.0f + static_cast<float>(dst[3]) * inv)), 0, 255));
}

// Solid alpha-over fill for the background box into the sprite accumulator,
// replicating fill_rounded_rect's geometry and corner-coverage law exactly
// (1 px anti-aliased edges from the corner circles, square when radius == 0).
void sprite_fill_rounded(uint8_t* rgba, const std::size_t stride, const int width,
                         const int height, int x0, int y0, int x1, int y1, const int radius,
                         const float r, const float g, const float b, const float a) {
    if (a <= 0.0f) return;
    x0 = std::clamp(x0, 0, width);
    y0 = std::clamp(y0, 0, height);
    x1 = std::clamp(x1, 0, width);
    y1 = std::clamp(y1, 0, height);
    if (x1 <= x0 || y1 <= y0) return;
    const int rad = std::max(0, std::min(radius, std::min((x1 - x0) / 2, (y1 - y0) / 2)));
    const int right = x1 - 1;
    const int bottom = y1 - 1;
    const float loop_r = static_cast<float>(rad) - 0.5f;
    const float hit_r = static_cast<float>(rad) + 0.5f;
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = rgba + static_cast<std::size_t>(y) * stride;
        for (int x = x0; x < x1; ++x) {
            float cov = 1.0f;
            if (rad > 0) {
                // Corner centres sit on the inner radius corners.
                const int cx = x < x0 + rad ? x0 + rad : (x > right - rad ? right - rad : -1);
                const int cy = y < y0 + rad ? y0 + rad : (y > bottom - rad ? bottom - rad : -1);
                if (cx >= 0 && cy >= 0) {
                    const float dx = static_cast<float>(x - cx);
                    const float dy = static_cast<float>(y - cy);
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d <= loop_r) {
                        cov = 1.0f;
                    } else if (d >= hit_r) {
                        cov = 0.0f;
                    } else {
                        cov = hit_r - d;
                    }
                }
            }
            if (cov <= 0.0f) continue;
            sprite_blend(rgba, stride, width, height, x, y, r, g, b, a * cov);
        }
    }
}

}  // namespace

// Separable box blur over an alpha plane (sliding window, clamped edges).
// radius <= 0 returns a copy.
std::vector<uint8_t> box_blur(const uint8_t* src, const int w, const int h,
                              const int radius) {
    if (w <= 0 || h <= 0) return {};
    if (radius <= 0) return std::vector<uint8_t>(src, src + static_cast<std::size_t>(w) * h);
    std::vector<uint8_t> tmp(static_cast<std::size_t>(w) * h);
    std::vector<uint8_t> dst(static_cast<std::size_t>(w) * h);
    const int n = 2 * radius + 1;
    for (int y = 0; y < h; ++y) {
        int acc = 0;
        for (int x = -radius; x <= radius; ++x)
            acc += src[static_cast<std::size_t>(y) * w + std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[static_cast<std::size_t>(y) * w + x] =
                static_cast<uint8_t>((acc + n / 2) / n);
            acc += src[static_cast<std::size_t>(y) * w + std::clamp(x + radius + 1, 0, w - 1)] -
                   src[static_cast<std::size_t>(y) * w + std::clamp(x - radius, 0, w - 1)];
        }
    }
    for (int x = 0; x < w; ++x) {
        int acc = 0;
        for (int y = -radius; y <= radius; ++y)
            acc += tmp[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            dst[static_cast<std::size_t>(y) * w + x] =
                static_cast<uint8_t>((acc + n / 2) / n);
            acc += tmp[static_cast<std::size_t>(std::clamp(y + radius + 1, 0, h - 1)) * w +
                       x] -
                   tmp[static_cast<std::size_t>(std::clamp(y - radius, 0, h - 1)) * w + x];
        }
    }
    return dst;
}

// Solid alpha-over fill for the background box, clipped to the canvas. A
// non-zero radius rounds the corners (0 = the plain square rect). Per-pixel
// coverage from the corner circles gives a 1 px anti-aliased edge.
void fill_rounded_rect(std::vector<uint8_t>& rgba, const int width, const int height,
                       const std::size_t stride, int x0, int y0, int x1, int y1,
                       const int radius, const int cr, const int cg, const int cb,
                       const float a) {
    if (a <= 0.0f) return;
    x0 = std::clamp(x0, 0, width);
    y0 = std::clamp(y0, 0, height);
    x1 = std::clamp(x1, 0, width);
    y1 = std::clamp(y1, 0, height);
    if (x1 <= x0 || y1 <= y0) return;
    const int rad = std::max(0, std::min(radius, std::min((x1 - x0) / 2, (y1 - y0) / 2)));
    const int right = x1 - 1;
    const int bottom = y1 - 1;
    const float loop_r = static_cast<float>(rad) - 0.5f;
    const float hit_r = static_cast<float>(rad) + 0.5f;
    const float inv = 1.0f - a;
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * stride;
        for (int x = x0; x < x1; ++x) {
            float cov = 1.0f;
            if (rad > 0) {
                // Corner centres sit on the inner radius corners.
                const int cx = x < x0 + rad ? x0 + rad : (x > right - rad ? right - rad : -1);
                const int cy = y < y0 + rad ? y0 + rad : (y > bottom - rad ? bottom - rad : -1);
                if (cx >= 0 && cy >= 0) {
                    const float dx = static_cast<float>(x - cx);
                    const float dy = static_cast<float>(y - cy);
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d <= loop_r) {
                        cov = 1.0f;
                    } else if (d >= hit_r) {
                        cov = 0.0f;
                    } else {
                        cov = hit_r - d;
                    }
                }
            }
            if (cov <= 0.0f) continue;
            const float aa = a * cov;
            uint8_t* dst = row + static_cast<std::size_t>(x) * 4u;
            dst[0] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::lround(cr * aa + dst[0] * (1.0f - aa))), 0, 255));
            dst[1] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::lround(cg * aa + dst[1] * (1.0f - aa))), 0, 255));
            dst[2] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::lround(cb * aa + dst[2] * (1.0f - aa))), 0, 255));
        }
    }
}

std::string find_font_path() {
    if (const char* e = std::getenv("CANVAS_TITLE_FONT"); e && *e && file_exists(e))
        return e;
    static const char* const kCandidates[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    };
    for (const char* const candidate : kCandidates)
        if (file_exists(candidate)) return candidate;
    return {};
}

std::vector<FontFace> installed_font_faces() {
    static std::mutex mu;
    static std::vector<FontFace> cache;
    static bool done = false;
    std::lock_guard<std::mutex> lock(mu);
    if (!done) {
        cache = scan_font_faces();
        done = true;
    }
    return cache;
}

std::vector<std::string> installed_font_families() {
    std::vector<std::string> out;
    for (const FontFace& f : installed_font_faces()) out.push_back(f.family);
    return out;
}

std::string find_font_path_for(const std::string& family) {
    const std::string want = ascii_lower(trim_ws(family));
    if (want.empty()) return find_font_path();
    for (const FontFace& f : installed_font_faces()) {
        if (ascii_lower(f.family) == want) return f.path;
    }
    // Unknown family degrades to the default face: a stale project still
    // renders instead of drawing nothing.
    return find_font_path();
}

int glyph_height(const Clip& clip, const int frame_height) noexcept {
    if (frame_height <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(clamp_size(clip.title.size) *
                                                    static_cast<float>(frame_height))));
}

Layout measure(const std::string& text, const int glyph_height_px,
               const std::string& font_path) {
    Layout out;
    if (text.empty() || glyph_height_px <= 0 || font_path.empty()) return out;
    const std::shared_ptr<const LoadedFont> font = load_font(font_path);
    if (!font || !font->ok) return out;
    const float scale = stbtt_ScaleForPixelHeight(&font->info, glyph_height_px);
    const GlyphLayout lay =
        layout_line(*font, codepoints(text), static_cast<double>(scale));
    if (lay.width <= 0 || lay.height <= 0) return out;
    out.width = lay.width;
    out.height = lay.height;
    return out;
}

namespace {

// Multi-line tight box, mirroring render_clip_title_with_font's stacking law
// (widest line = block width; 0.25 em gap between lines — so the fit law and
// the rasteriser agree pixel-for-pixel on what "the block" is).
Layout measure_block(const std::string& text, const int glyph_height_px,
                     const std::string& font_path) {
    Layout out;
    if (text.empty() || glyph_height_px <= 0 || font_path.empty()) return out;
    const std::shared_ptr<const LoadedFont> font = load_font(font_path);
    if (!font || !font->ok) return out;
    const float scale = stbtt_ScaleForPixelHeight(&font->info, glyph_height_px);
    const int gap = std::max(0, static_cast<int>(std::lround(glyph_height_px * 0.25f)));
    const std::vector<std::string> lines = split_lines(text);
    int total = std::max(0, static_cast<int>(lines.size()) - 1) * gap;
    int max_w = 0;
    for (const std::string& line : lines) {
        const GlyphLayout lay = layout_line(*font, codepoints(line), static_cast<double>(scale));
        if (lay.width <= 0 || lay.height <= 0) continue;
        max_w = std::max(max_w, lay.width);
        total += lay.height;
    }
    out.width = max_w;
    out.height = total;
    return out;
}

}  // namespace

SubtitleFit fit_caption(const std::string& text, const int frame_width, const int frame_height,
                        const std::string& font_path, const float size_hint) {
    SubtitleFit out;
    out.size = clamp_size(size_hint);
    const int margin =
        frame_height > 0 ? std::max(1, static_cast<int>(std::lround(
                                           frame_height * kFitBottomMarginFraction)))
                         : 0;
    if (frame_width <= 0 || frame_height <= 0) {
        out.pos_y = static_cast<double>(margin);
        return out;
    }
    // Nominal fallback block for the bottom anchor when measurement fails
    // (empty text / missing font): a single line one em tall reads as the
    // subtitle safe-zone target.
    auto nominal_height = [&]() {
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(out.size) *
                                                        frame_height * 1.25)));
    };

    const int max_w = std::max(1, static_cast<int>(frame_width * kFitMaxWidthFraction));
    const int max_h = std::max(1, static_cast<int>(frame_height * kFitMaxHeightFraction));
    const auto block_at = [&](const float size) -> Layout {
        const int g = std::max(1, static_cast<int>(std::lround(size * frame_height)));
        return measure_block(text, g, font_path);
    };

    // Shrink (binary search over the size fraction) until the block fits both
    // safe areas. Monotone: smaller size ⇒ narrower and shorter. Clamped to
    // kSizeMin — an unbreakably long token accepts its overflow.
    if (!(block_at(out.size).width <= max_w && block_at(out.size).height <= max_h)) {
        float lo = kSizeMin;
        float hi = out.size;
        for (int i = 0; i < 48; ++i) {
            const float mid = (lo + hi) * 0.5f;
            const Layout b = block_at(mid);
            if (b.width <= 0 || b.height <= 0 || (b.width <= max_w && b.height <= max_h))
                lo = mid;
            else
                hi = mid;
        }
        out.size = lo;
    }

    const Layout final = block_at(out.size);
    const int block_h = final.height;
    out.block_width = final.width;
    out.block_height = block_h;
    // Centre the block, then shift it down so its bottom sits `margin` px above
    // the frame bottom (the classic subtitle anchor). When the block is
    // unmeasurable (missing font / empty text, block_height == 0) anchor a
    // nominal single-line height instead so pos_y still reads as a sane offset.
    const int anchor_h = block_h > 0 ? block_h : nominal_height();
    out.pos_y = (frame_height - anchor_h) / 2.0 - static_cast<double>(margin);
    if (out.pos_y < 0) out.pos_y = 0.0;
    return out;
}

void render_clip_title_with_font(const Clip& clip, const std::string& font_path,
                                 std::vector<uint8_t>& rgba, const int width,
                                 const int height, const std::size_t stride) {
    if (width <= 0 || height <= 0 || !clip.has_title() || font_path.empty()) return;
    if (rgba.size() < stride * static_cast<std::size_t>(height)) return;

    const std::shared_ptr<const LoadedFont> font = load_font(font_path);
    if (!font || !font->ok) return;

    const float em_px = static_cast<float>(glyph_height(clip, height));
    const float scale = stbtt_ScaleForPixelHeight(&font->info, em_px);

    const std::vector<std::string> lines = split_lines(clip.title.text);
    std::vector<LineBitmap> bits;
    bits.reserve(lines.size());
    const TextStyle style{clip.title.bold, clip.title.italic, clip.title.underline};
    for (const std::string& line : lines)
        bits.push_back(rasterize_line(*font, codepoints(line), scale, style, em_px));

    const float opacity = std::clamp(clip.opacity, 0.0f, 1.0f);
    const float text_a = std::clamp(clip.title.a, 0.0f, 1.0f);
    const int cr = static_cast<int>(std::lround(std::clamp(clip.title.r, 0.0f, 1.0f) * 255.0f));
    const int cg = static_cast<int>(std::lround(std::clamp(clip.title.g, 0.0f, 1.0f) * 255.0f));
    const int cb = static_cast<int>(std::lround(std::clamp(clip.title.b, 0.0f, 1.0f) * 255.0f));

    // The clip's visual position nudges the whole title block (px). Zero by
    // default, so untouched titles render exactly centred as before; the
    // Subtitles tab's position sliders and the Video tab's Position row edit
    // these same fields.
    const int off_x = static_cast<int>(std::lround(clip.pos_x));
    const int off_y = static_cast<int>(std::lround(clip.pos_y));

    std::vector<Placed> placed = place_block(bits, width, height, off_x, off_y, em_px);
    if (placed.empty()) return;

    // Background box behind the whole block (behind glyphs and shadows).
    if (clip.title.box) {
        int x0 = width, y0 = height, x1 = 0, y1 = 0;
        for (const Placed& pl : placed) {
            x0 = std::min(x0, pl.x);
            y0 = std::min(y0, pl.y);
            x1 = std::max(x1, pl.x + pl.w);
            y1 = std::max(y1, pl.y + pl.h);
        }
        const int px = static_cast<int>(std::lround(clip.title.box_pad_x));
        const int py = static_cast<int>(std::lround(clip.title.box_pad_y));
        const int brad = std::max(0, static_cast<int>(std::lround(clip.title.box_radius)));
        const int bcr = static_cast<int>(
            std::lround(std::clamp(clip.title.box_r, 0.0f, 1.0f) * 255.0f));
        const int bcg = static_cast<int>(
            std::lround(std::clamp(clip.title.box_g, 0.0f, 1.0f) * 255.0f));
        const int bcb = static_cast<int>(
            std::lround(std::clamp(clip.title.box_b, 0.0f, 1.0f) * 255.0f));
        fill_rounded_rect(rgba, width, height, stride, x0 - px, y0 - py, x1 + px, y1 + py,
                          brad, bcr, bcg, bcb, clip.title.box_opacity * opacity);
    }

    // Drop shadow (blurred, tinted, offset copy of each line) first, then the
    // glyphs over it. The shadow inherits the text alpha so faded text casts
    // a faded shadow.
    const int scr = static_cast<int>(
        std::lround(std::clamp(clip.title.shadow_r, 0.0f, 1.0f) * 255.0f));
    const int scg = static_cast<int>(
        std::lround(std::clamp(clip.title.shadow_g, 0.0f, 1.0f) * 255.0f));
    const int scb = static_cast<int>(
        std::lround(std::clamp(clip.title.shadow_b, 0.0f, 1.0f) * 255.0f));
    const int sdx = static_cast<int>(std::lround(clip.title.shadow_dx));
    const int sdy = static_cast<int>(std::lround(clip.title.shadow_dy));
    const int srad = std::max(0, static_cast<int>(std::lround(clip.title.shadow_blur)));
    for (const Placed& pl : placed) {
        if (clip.title.shadow) {
            const std::vector<uint8_t> salpha =
                box_blur(pl.bmp->alpha.data(), pl.w, pl.h, srad);
            blit_alpha(salpha.data(), pl.w, pl.h, pl.x + sdx, pl.y + sdy, rgba, width,
                       height, stride, scr, scg, scb, clip.title.shadow_opacity * text_a,
                       opacity);
        }
        blit_alpha(pl.bmp->alpha.data(), pl.w, pl.h, pl.x, pl.y, rgba, width, height,
                   stride, cr, cg, cb, text_a, opacity);
    }
}

void render_clip_title(const Clip& clip, std::vector<uint8_t>& rgba, const int width,
                       const int height, const std::size_t stride) {
    if (!clip.has_title()) return;
    const std::string path = find_font_path_for(clip.title.font_family);
    if (path.empty()) return;
    render_clip_title_with_font(clip, path, rgba, width, height, stride);
}

// Premultiplied-RGBA8 tight sprite of clip.title for the GPU fast path (see
// title.hpp). Rasterises into a full-canvas accumulator with the same
// layering/ordering law as render_clip_title_with_font (box -> shadow ->
// glyphs, no offsets drop out of the canvas box), tracks the coverage
// footprint, then crops to the tight box. Blending the sprite over an opaque
// canvas reproduces the CPU compose save for the final rounding pass.
TitleSprite raster_title_sprite(const Clip& clip, const int canvas_w, const int canvas_h,
                                const std::string& font_path) {
    TitleSprite out;
    if (canvas_w <= 0 || canvas_h <= 0 || !clip.has_title() || font_path.empty()) return out;

    const std::shared_ptr<const LoadedFont> font = load_font(font_path);
    if (!font || !font->ok) return out;

    const float em_px = static_cast<float>(glyph_height(clip, canvas_h));
    const float scale = stbtt_ScaleForPixelHeight(&font->info, em_px);

    const std::vector<std::string> lines = split_lines(clip.title.text);
    std::vector<LineBitmap> bits;
    bits.reserve(lines.size());
    const TextStyle style{clip.title.bold, clip.title.italic, clip.title.underline};
    for (const std::string& line : lines)
        bits.push_back(rasterize_line(*font, codepoints(line), scale, style, em_px));

    const float opacity = std::clamp(clip.opacity, 0.0f, 1.0f);
    const float text_a = std::clamp(clip.title.a, 0.0f, 1.0f);
    const float cr = static_cast<float>(std::lround(std::clamp(clip.title.r, 0.0f, 1.0f) * 255.0f));
    const float cg = static_cast<float>(std::lround(std::clamp(clip.title.g, 0.0f, 1.0f) * 255.0f));
    const float cb = static_cast<float>(std::lround(std::clamp(clip.title.b, 0.0f, 1.0f) * 255.0f));
    const int off_x = static_cast<int>(std::lround(clip.pos_x));
    const int off_y = static_cast<int>(std::lround(clip.pos_y));

    // Same placement law as the canvas renderer, so a sprite and a CPU render
    // put the block at the same pixels no matter which path exports.
    const std::vector<Placed> placed = place_block(bits, canvas_w, canvas_h, off_x, off_y, em_px);
    if (placed.empty()) return out;

    // Full-canvas accumulator: premultiplied colour in RGB, coverage in A —
    // prepared exactly like a CPU alpha-over, so compositing the packed sprite
    // over an opaque background reproduces render_clip_title_with_font's output
    // (up to the single final blend rounding).
    std::vector<uint8_t> acc(static_cast<std::size_t>(canvas_w) * canvas_h * 4u, 0);
    const std::size_t stride = static_cast<std::size_t>(canvas_w) * 4u;

    // Background box behind the whole block (behind glyphs and shadows).
    if (clip.title.box) {
        int x0 = canvas_w, y0 = canvas_h, x1 = 0, y1 = 0;
        for (const Placed& pl : placed) {
            x0 = std::min(x0, pl.x);
            y0 = std::min(y0, pl.y);
            x1 = std::max(x1, pl.x + pl.w);
            y1 = std::max(y1, pl.y + pl.h);
        }
        const int px = static_cast<int>(std::lround(clip.title.box_pad_x));
        const int py = static_cast<int>(std::lround(clip.title.box_pad_y));
        const int brad = std::max(0, static_cast<int>(std::lround(clip.title.box_radius)));
        const float bcr = static_cast<float>(
            std::lround(std::clamp(clip.title.box_r, 0.0f, 1.0f) * 255.0f));
        const float bcg = static_cast<float>(
            std::lround(std::clamp(clip.title.box_g, 0.0f, 1.0f) * 255.0f));
        const float bcb = static_cast<float>(
            std::lround(std::clamp(clip.title.box_b, 0.0f, 1.0f) * 255.0f));
        sprite_fill_rounded(acc.data(), stride, canvas_w, canvas_h, x0 - px, y0 - py, x1 + px,
                            y1 + py, brad, bcr, bcg, bcb, clip.title.box_opacity * opacity);
    }

    // Drop shadow (blurred, tinted, offset copy of each line) first, then the
    // glyphs over it. The shadow inherits the text alpha so faded text casts a
    // faded shadow.
    const float scr = static_cast<float>(
        std::lround(std::clamp(clip.title.shadow_r, 0.0f, 1.0f) * 255.0f));
    const float scg = static_cast<float>(
        std::lround(std::clamp(clip.title.shadow_g, 0.0f, 1.0f) * 255.0f));
    const float scb = static_cast<float>(
        std::lround(std::clamp(clip.title.shadow_b, 0.0f, 1.0f) * 255.0f));
    const int sdx = static_cast<int>(std::lround(clip.title.shadow_dx));
    const int sdy = static_cast<int>(std::lround(clip.title.shadow_dy));
    const int srad = std::max(0, static_cast<int>(std::lround(clip.title.shadow_blur)));
    const float shadow_a = clip.title.shadow_opacity * text_a * opacity;
    const float glyph_a = text_a * opacity;
    for (const Placed& pl : placed) {
        if (clip.title.shadow) {
            const std::vector<uint8_t> salpha = box_blur(pl.bmp->alpha.data(), pl.w, pl.h, srad);
            for (int py = 0; py < pl.h; ++py) {
                const float sa_scale = shadow_a / 255.0f;
                for (int px2 = 0; px2 < pl.w; ++px2) {
                    const uint8_t sa = salpha[static_cast<std::size_t>(py) * pl.w + px2];
                    if (sa == 0) continue;
                    sprite_blend(acc.data(), stride, canvas_w, canvas_h, pl.x + sdx + px2,
                                 pl.y + sdy + py, scr, scg, scb,
                                 static_cast<float>(sa) * sa_scale);
                }
            }
        }
        for (int py = 0; py < pl.h; ++py) {
            const float ga_scale = glyph_a / 255.0f;
            for (int px2 = 0; px2 < pl.w; ++px2) {
                const uint8_t ga = pl.bmp->alpha[static_cast<std::size_t>(py) * pl.w + px2];
                if (ga == 0) continue;
                sprite_blend(acc.data(), stride, canvas_w, canvas_h, pl.x + px2, pl.y + py, cr,
                             cg, cb, static_cast<float>(ga) * ga_scale);
            }
        }
    }

    // Tight crop of the coverage footprint.
    int min_x = canvas_w, min_y = canvas_h, max_x = -1, max_y = -1;
    for (int y = 0; y < canvas_h; ++y) {
        const uint8_t* row = acc.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < canvas_w; ++x) {
            if (row[x * 4u + 3u] != 0) {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }
    if (max_x < 0) return out;

    out.ox = min_x;
    out.oy = min_y;
    out.width = max_x - min_x + 1;
    out.height = max_y - min_y + 1;
    out.data.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.data.data() + static_cast<std::size_t>(y) * out.width * 4u,
                    acc.data() + static_cast<std::size_t>(min_y + y) * stride +
                        static_cast<std::size_t>(min_x) * 4u,
                    static_cast<std::size_t>(out.width) * 4u);
    }
    return out;
}

}  // namespace canvas::core::title