#include "canvas/core/timeline/blend.hpp"

#include "canvas/core/grade_graph/composite.hpp"

#include <algorithm>
#include <cmath>

namespace canvas::core::blend {

namespace {

// Map the timeline enum (Normal, Add, Multiply, Screen, Overlay, SoftLight,
// Subtract, Difference) onto the color-page enum, which stores the same mode
// names in W3C order. The float law only knows the W3C enum, so this mapping is
// the single place the two orderings meet.
grade_graph::BlendMode to_grade(const BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Normal: return grade_graph::BlendMode::kNormal;
        case BlendMode::Add: return grade_graph::BlendMode::kAdd;
        case BlendMode::Multiply: return grade_graph::BlendMode::kMultiply;
        case BlendMode::Screen: return grade_graph::BlendMode::kScreen;
        case BlendMode::Overlay: return grade_graph::BlendMode::kOverlay;
        case BlendMode::SoftLight: return grade_graph::BlendMode::kSoftLight;
        case BlendMode::Subtract: return grade_graph::BlendMode::kSubtract;
        case BlendMode::Difference: return grade_graph::BlendMode::kDifference;
    }
    return grade_graph::BlendMode::kNormal;
}

}  // namespace

const char* blend_mode_name(const BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Normal: return "Normal";
        case BlendMode::Add: return "Add";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::Screen: return "Screen";
        case BlendMode::Overlay: return "Overlay";
        case BlendMode::SoftLight: return "Soft Light";
        case BlendMode::Subtract: return "Subtract";
        case BlendMode::Difference: return "Difference";
    }
    return "Normal";
}

uint8_t blend_channel(const BlendMode mode, const float opacity, const uint8_t base,
                      const uint8_t src) noexcept {
    const float b = static_cast<float>(base) / 255.0f;
    const float s = static_cast<float>(src) / 255.0f;
    const float blended = grade_graph::blend_channel(to_grade(mode), b, s);
    const float out = blended * opacity + b * (1.0f - opacity);
    const float scaled = out * 255.0f + 0.5f;  // rounded to nearest byte
    return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

}  // namespace canvas::core::blend