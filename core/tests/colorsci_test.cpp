// Phase 1 tests for the primary-grade math law (colorsci/wheels.hpp + cdl.hpp).
// Verifies identity-at-defaults, the specified curve shapes, the luma-
// preservation invariants that keep sat/boost/hue/lum-mix self-consistent, and
// the closed-form LGG<->CDL conversion. Headless — links only canvas_core.

#include "canvas/core/colorsci/cdl.hpp"
#include "canvas/core/colorsci/wheels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cs = canvas::core::colorsci;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

constexpr float kClose = 1e-4f;

bool near(float a, float b) {
    return std::fabs(a - b) < kClose;
}

bool rgb_near(const cs::RGBF& a, const cs::RGBF& b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

float chroma_mag(const cs::RGBF& c) {
    return std::max(std::max(c.r, c.g), c.b) - std::min(std::min(c.r, c.g), c.b);
}

void test_lgg() {
    // Identity at defaults.
    const cs::RGBF v{0.11f, 0.71f, 0.23f};
    check(rgb_near(cs::apply_lgg(v, {}), v), "lgg identity at defaults");

    // With gain=gamma=1 the law is out = in + lift*(1 - in): y(0)=lift, y(1)=1.
    cs::LGG lift_only;
    lift_only.lift_master = 0.4f;
    const cs::RGBF out_lift = cs::apply_lgg({0.0f, 0.0f, 0.0f}, lift_only);
    check(near(out_lift.r, 0.4f), "lgg lift raises black to lift value");
    check(near(cs::apply_lgg({1.0f, 1.0f, 1.0f}, lift_only).r, 1.0f),
          "lgg lift tapers to no effect at white");

    // Gain scales the whole signal: y(1) = gain.
    cs::LGG gain_only;
    gain_only.gain_master = 1.5f;
    const cs::RGBF out_gain = cs::apply_lgg({1.0f, 1.0f, 1.0f}, gain_only);
    check(near(out_gain.g, 1.5f), "lgg gain scales white");

    // Gamma reshapes midtones: with gain=lift=0, out = in^(1/gamma).
    cs::LGG gamma_only;
    gamma_only.gamma_master = 2.0f;  // sqrt curve
    const cs::RGBF out_gamma = cs::apply_lgg({0.25f, 0.25f, 0.25f}, gamma_only);
    check(near(out_gamma.r, 0.5f), "lgg gamma 2.0 -> sqrt curve at 0.25");

    // Strong negative lift must clamp the pow base, never produce NaN.
    cs::LGG neg_lift;
    neg_lift.lift_master = -3.0f;
    const cs::RGBF out_neg = cs::apply_lgg(v, neg_lift);
    const bool finite = std::isfinite(out_neg.r) && std::isfinite(out_neg.g) &&
                        std::isfinite(out_neg.b);
    check(finite && out_neg.r >= 0.0f && out_neg.r <= 1.0f, "lgg negative lift clamped, no NaN");

    // Per-channel offset biases only that channel's curve outcome.
    cs::LGG channel;
    channel.gamma_master = 1.0f;
    channel.gain_master = 1.0f;
    channel.lift_b = 0.5f;
    const cs::RGBF out_ch = cs::apply_lgg(v, channel);
    check(near(out_ch.b, v.b + 0.5f * (1.0f - v.b)), "lgg per-channel lift applies to channel b");
    check(near(out_ch.r, v.r) && near(out_ch.g, v.g), "lgg per-channel lift leaves others alone");
}

void test_offset_contrast() {
    const cs::RGBF v{0.35f, 0.55f, 0.75f};

    cs::Offset o;
    o.master = 0.1f;
    const cs::RGBF out = cs::apply_offset(v, o);
    check(near(out.r, v.r + 0.1f) && near(out.g, v.g + 0.1f) && near(out.b, v.b + 0.1f),
          "offset adds uniformly (moves black AND white)");
    check(rgb_near(cs::apply_offset(v, {}), v), "offset identity at defaults");

    // Contrast pivots around the fixed point; pivot is unmoved, extremes widen.
    const cs::RGBF at_pivot = cs::apply_contrast_pivot({0.435f, 0.435f, 0.435f}, 2.5f, 0.435f);
    check(near(at_pivot.r, 0.435f), "contrast keeps the pivot fixed");
    const cs::RGBF widen = cs::apply_contrast_pivot({0.3f, 0.3f, 0.3f}, 2.0f, 0.435f);
    check(near(widen.r, 0.435f + (0.3f - 0.435f) * 2.0f), "contrast 2 widens distance from pivot");
    check(rgb_near(cs::apply_contrast_pivot(v, 1.0f, 0.435f), v), "contrast identity at 1");
}

void test_saturation() {
    const cs::RGBF v{0.2f, 0.5f, 0.8f};
    const float L = cs::luma(v);

    check(rgb_near(cs::apply_saturation(v, 1.0f), v), "saturation 1 is identity");

    const cs::RGBF mono = cs::apply_saturation(v, 0.0f);
    check(near(mono.r, L) && near(mono.g, L) && near(mono.b, L), "saturation 0 collapses to luma");

    const cs::RGBF boosted = cs::apply_saturation(v, 2.0f);
    check(near(cs::luma(boosted), L), "saturation preserves luma");
    check(near(chroma_mag(boosted), 2.0f * chroma_mag(v)), "saturation 2 doubles chroma");
}

void test_hue_rotate() {
    const cs::RGBF v{0.22f, 0.63f, 0.31f};

    check(rgb_near(cs::apply_hue_rotate(v, 0.0f), v), "hue rotate 0 is identity");

    // Luma invariance across angles is the invariant that makes the rotate
    // chroma-vector law stable (weighted R- and B-gain chroma, G derived).
    for (float deg = -135.0f; deg <= 135.0f; deg += 15.0f) {
        const cs::RGBF r = cs::apply_hue_rotate(v, deg);
        check(near(cs::luma(r), cs::luma(v)),
              "hue rotate preserves luma at every angle");
    }

    const cs::RGBF full = cs::apply_hue_rotate(v, 360.0f);
    check(rgb_near(full, v), "hue rotate 360 returns to identity");

    // A pure chroma vector rotates through the hue ring, then back.
    const cs::RGBF test{1.0f, 0.1f, 0.1f};
    const cs::RGBF rt = cs::apply_hue_rotate(cs::apply_hue_rotate(test, 90.0f), -90.0f);
    check(rgb_near(rt, test), "hue rotate round-trips (positive then negative)");
}

void test_color_boost_zoned() {
    const cs::RGBF lowsat{0.49f, 0.50f, 0.52f};  // chroma ~0.03
    const cs::RGBF highsat{1.0f, 0.0f, 0.0f};    // chroma = 1

    check(rgb_near(cs::apply_color_boost(lowsat, 0.0f), lowsat), "color boost 0 is identity");
    check(near(chroma_mag(cs::apply_color_boost(highsat, 1.0f)), chroma_mag(highsat)),
          "color boost leaves fully saturated pixels alone");
    const float dlowsat = chroma_mag(cs::apply_color_boost(lowsat, 1.0f)) - chroma_mag(lowsat);
    check(dlowsat > 1e-3f, "color boost lifts low-saturation chroma");

    const cs::RGBF dark{0.02f, 0.05f, 0.03f};   // low luma WITH chroma
    const cs::RGBF bright{0.98f, 0.9f, 0.94f};  // high luma WITH chroma
    const auto zone_sat = [](const cs::RGBF& p, float shadow, float highlight) {
        return cs::apply_saturation_zoned(p, 1.0f, shadow, highlight);
    };
    // Near-black sits in the shadow zone: the shadow term moves it, the
    // highlight term does not.
    check(chroma_mag(zone_sat(dark, 1.0f, 0.0f)) - chroma_mag(dark) > 1e-3f,
          "shadows term desaturates near-black pixels");
    check(near(chroma_mag(zone_sat(dark, 0.0f, 1.0f)) - chroma_mag(dark), 0.0f),
          "highlight term leaves near-black pixels alone");
    // Near-white is the mirror image.
    check(near(chroma_mag(zone_sat(bright, 1.0f, 0.0f)) - chroma_mag(bright), 0.0f),
          "shadow term leaves near-white pixels alone");
    check(chroma_mag(zone_sat(bright, 0.0f, -1.0f)) - chroma_mag(bright) < -1e-3f,
          "highlight term can desaturate near-white pixels");
    check(rgb_near(zone_sat(dark, 0.0f, 0.0f), dark), "zoned saturation identity at 0,0");
}

void test_lum_mix() {
    const cs::RGBF original{0.15f, 0.45f, 0.75f};
    // A pure saturation correction: luma unchanged, chroma doubled.
    const cs::RGBF corrected = cs::apply_saturation(original, 2.0f);

    check(rgb_near(cs::apply_lum_mix(corrected, original, 1.0f), corrected),
          "lum mix 1 keeps the full correction");
    check(rgb_near(cs::apply_lum_mix(corrected, original, 0.0f), original),
          "lum mix 0 of a saturation-only correction is a no-op");
    // A luminance-only correction survives even at lum mix 0.
    const cs::RGBF brightened = cs::apply_contrast_pivot(original, 1.4f, 0.435f);
    const cs::RGBF only_luma = cs::apply_lum_mix(brightened, original, 0.0f);
    check(near(cs::luma(only_luma), cs::luma(brightened)), "lum mix 0 keeps corrected luma");
    check(near(chroma_mag(only_luma), chroma_mag(original)), "lum mix 0 keeps original chroma");
}

void test_hue_ring_puck() {
    const cs::RGBF red = cs::hue_to_rgb(0.0f);
    check(near(red.r, 1.0f) && near(red.g, -0.5f) && near(red.b, -0.5f),
          "hue_to_rgb 0 is the red pull (+1 R, even cyan bleed)");
    check(near(cs::hue_to_rgb(180.0f).r, -1.0f), "hue_to_rgb 180 pulls red negative (cyan)");
    check(rgb_near(cs::hue_to_rgb(360.0f), red), "hue_to_rgb 360 wraps to 0");
    check(near(cs::hue_to_rgb(60.0f).r, cs::hue_to_rgb(60.0f).g) &&
              cs::hue_to_rgb(60.0f).r > 0.0f && cs::hue_to_rgb(60.0f).b < 0.0f,
          "hue_to_rgb 60 is a balanced yellow pull (positive R+G, negative B)");
    const cs::RGBF h = cs::hue_to_rgb(123.0f);
    check(near(std::sqrt(h.r * h.r + h.g * h.g + h.b * h.b), std::sqrt(1.5f)),
          "hue_to_rgb magnitude is constant (cos^2 sum = 3/2)");
    check(near(cs::hue_to_rgb(120.0f).g, 1.0f), "hue_to_rgb 120 leads with green");

    check(rgb_near(cs::puck_xy_to_offset(0.0f, 0.0f), cs::RGBF{}), "puck center is zero offset");
    const cs::RGBF east = cs::puck_xy_to_offset(1.0f, 0.0f);
    check(near(east.r, 1.0f) && near(east.g, -0.5f), "puck full-right is a red-dominant offset");
    check(near(cs::puck_xy_to_offset(0.5f, 0.0f).r, 0.5f), "puck radius scales the offset");
    check(cs::puck_xy_to_offset(-1.0f, 0.0f).r < 0.0f, "puck full-left is a cyan (negative red) pull");
}

void test_cdl() {
    const cs::RGBF v{0.2f, 0.5f, 0.8f};
    check(rgb_near(cs::apply_cdl(v, {}), v), "cdl identity at defaults");

    cs::Cdl add;
    add.offset_g = 0.25f;
    check(near(cs::apply_cdl(v, add).g, v.g + 0.25f), "cdl offset is additive (power 1)");

    cs::Cdl pwr;
    pwr.power_r = 2.0f;
    const cs::RGBF shaped = cs::apply_cdl({0.5f, 0.5f, 0.5f}, pwr);
    check(near(shaped.r, 0.25f), "cdl power squares");
    check(near(shaped.g, 0.5f) && near(shaped.b, 0.5f),
          "cdl single-channel power does not leak across channels");

    cs::Cdl neg;
    neg.offset_r = -0.7f;
    neg.slope_r = 1.0f;
    const cs::RGBF out_neg = cs::apply_cdl({0.5f, 0.5f, 0.5f}, neg);
    check(out_neg.r >= 0.0f && std::isfinite(out_neg.r),
          "cdl negative base clamps to zero, no NaN");

    cs::Cdl desat;
    desat.sat = 0.0f;
    const cs::RGBF mono = cs::apply_cdl(v, desat);
    const float L = cs::luma(cs::apply_cdl(v, {}));
    check(near(mono.r, L) && near(mono.g, L) && near(mono.b, L), "cdl sat 0 collapses to luma");
}

void test_lgg_cdl_conversion() {
    // lift == 0: closed form, and the two models evaluate identically.
    cs::LGG l;
    l.gain_master = 1.35f;
    l.gamma_master = 1.8f;
    l.gain_b = 0.9f;
    const cs::CdlConversion conv = cs::cdl_from_lgg(l);
    check(conv.exact, "cdl_from_lgg exact when lift == 0");
    check(near(conv.cdl.slope_r, l.gain_master), "cdl slope mirrors gain");
    check(near(conv.cdl.power_r, 1.0f / l.gamma_master), "cdl power mirrors 1/gamma");
    for (float x = 0.0f; x <= 1.0f; x += 0.13f) {
        const cs::RGBF src{x, 0.3f, 0.7f};
        check(rgb_near(cs::apply_lgg(src, l), cs::apply_cdl(src, conv.cdl)),
              "lgg==cdl at lift 0 agrees across the range");
    }

    // Non-zero lift: conversion reports inexact rather than approximating.
    l.lift_master = 0.2f;
    check(!cs::cdl_from_lgg(l).exact, "cdl_from_lgg flags inexact when lift != 0");
}

void test_preview_path_smoke() {
    // The wheel set applied top-to-bottom on a synthetic pixel: offset before
    // LGG, then contrast/pivot, then saturation, then hue, boost, lum mix so
    // the full Phase-1 chain terminates finite on white and black.
    cs::LGG w;
    w.lift_master = 0.05f;
    w.gamma_master = 1.1f;
    w.gain_master = 1.02f;
    cs::Offset off;
    off.master = 0.02f;

    const auto chain = [&](const cs::RGBF& src) {
        const cs::RGBF lifted = cs::apply_lgg(cs::apply_offset(src, off), w);
        const cs::RGBF shaped = cs::apply_contrast_pivot(lifted, 1.15f, cs::kDefaultPivot);
        const cs::RGBF sat = cs::apply_saturation_zoned(shaped, 1.0f, 0.0f, 0.0f);
        const cs::RGBF hue = cs::apply_hue_rotate(sat, -7.0f);
        const cs::RGBF boosted = cs::apply_color_boost(hue, 0.4f);
        return cs::apply_lum_mix(boosted, src, 0.8f);
    };

    const cs::RGBF white = chain({1.0f, 1.0f, 1.0f});
    const cs::RGBF black = chain({0.0f, 0.0f, 0.0f});
    const cs::RGBF mix = chain({1.0f, 0.5f, 0.0f});
    const bool finite = std::isfinite(white.r) && std::isfinite(black.r) && std::isfinite(mix.r) &&
                        std::isfinite(white.g) && std::isfinite(mix.b);
    check(finite, "full wheel chain stays finite on white and black");
    check(mix.r > 0.0f && mix.b < 1.0f, "full wheel chain changes the pixel");
}

}  // namespace

int main() {
    test_lgg();
    test_offset_contrast();
    test_saturation();
    test_hue_rotate();
    test_color_boost_zoned();
    test_lum_mix();
    test_hue_ring_puck();
    test_cdl();
    test_lgg_cdl_conversion();
    test_preview_path_smoke();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}