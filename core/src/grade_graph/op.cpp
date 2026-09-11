#include "canvas/core/grade_graph/op.hpp"

namespace canvas::core::grade_graph {

namespace {

constexpr struct OpEntry {
    const char* name;
    OpKind value;
} kOps[] = {
    {"identity", OpKind::kIdentity},
    {"lgg", OpKind::kLgg},
    {"cdl", OpKind::kCdl},
    {"curves", OpKind::kCurves},
};

[[nodiscard]] bool lgg_is_identity(const colorsci::LGG& p) noexcept {
    return p.lift_master == 0.0f && p.gamma_master == 1.0f && p.gain_master == 1.0f &&
           p.lift_r == 0.0f && p.lift_g == 0.0f && p.lift_b == 0.0f &&
           p.gamma_r == 1.0f && p.gamma_g == 1.0f && p.gamma_b == 1.0f &&
           p.gain_r == 1.0f && p.gain_g == 1.0f && p.gain_b == 1.0f;
}

[[nodiscard]] bool offset_is_identity(const colorsci::Offset& o) noexcept {
    return o.master == 0.0f && o.r == 0.0f && o.g == 0.0f && o.b == 0.0f;
}

[[nodiscard]] bool cdl_is_identity(const colorsci::Cdl& c) noexcept {
    return c.slope_r == 1.0f && c.slope_g == 1.0f && c.slope_b == 1.0f &&
           c.offset_r == 0.0f && c.offset_g == 0.0f && c.offset_b == 0.0f &&
           c.power_r == 1.0f && c.power_g == 1.0f && c.power_b == 1.0f &&
           c.sat == 1.0f;
}

}  // namespace

colorsci::RGBF op_apply(const Node& node, const colorsci::RGBF& in) {
    switch (node.correct_mode) {
        case OpKind::kLgg: {
            colorsci::RGBF out = in;
            if (node.offset) out = colorsci::apply_offset(out, *node.offset);
            return colorsci::apply_lgg(out, node.lgg.value_or(colorsci::LGG{}));
        }
        case OpKind::kCdl:
            return colorsci::apply_cdl(in, node.cdl.value_or(colorsci::Cdl{}));
        case OpKind::kCurves:
            return colorsci::apply_curves(in, node.curves.value_or(colorsci::CurveParams{}));
        case OpKind::kIdentity:
            break;
    }
    return in;
}

bool op_is_identity(const Node& node) {
    switch (node.correct_mode) {
        case OpKind::kLgg:
            if (node.offset && !offset_is_identity(*node.offset)) return false;
            return !node.lgg || lgg_is_identity(*node.lgg);
        case OpKind::kCdl:
            return !node.cdl || cdl_is_identity(*node.cdl);
        case OpKind::kCurves:
            return !node.curves || node.curves->is_identity();
        case OpKind::kIdentity:
            return true;
    }
    return true;
}

const char* op_name(OpKind kind) {
    for (const auto& e : kOps) {
        if (e.value == kind) return e.name;
    }
    return "identity";
}

OpKind op_from_name(const std::string& name) {
    for (const auto& e : kOps) {
        if (name == e.name) return e.value;
    }
    return OpKind::kIdentity;
}

}  // namespace canvas::core::grade_graph