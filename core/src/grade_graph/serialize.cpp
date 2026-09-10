#include "canvas/core/grade_graph/serialize.hpp"

#include <span>
#include <string>

namespace canvas::core::grade_graph {

namespace {

using json = nlohmann::json;

// --- string-enum mapping (forward-compatible: unknown -> default) ------------

template <typename E>
struct EnumStrings {
    const char* name;
    E value;
};

std::span<const EnumStrings<NodeKind>> kind_table() {
    static const EnumStrings<NodeKind> k[] = {
        {"corrector", NodeKind::kCorrector},
        {"parallel", NodeKind::kParallel},
        {"layer", NodeKind::kLayer},
        {"outside", NodeKind::kOutside},
        {"key_mixer", NodeKind::kKeyMixer},
        {"splitter", NodeKind::kSplitter},
        {"combiner", NodeKind::kCombiner},
        {"parallel_mixer", NodeKind::kParallelMixer},
        {"layer_mixer", NodeKind::kLayerMixer},
        {"output", NodeKind::kOutput},
    };
    return k;
}

std::span<const EnumStrings<PipeType>> pipe_table() {
    static const EnumStrings<PipeType> k[] = {
        {"rgb", PipeType::kRgb},
        {"key", PipeType::kKey},
        {"channel", PipeType::kChannel},
    };
    return k;
}

std::span<const EnumStrings<CorrectMode>> mode_table() {
    static const EnumStrings<CorrectMode> k[] = {
        {"identity", CorrectMode::kIdentity},
        {"lgg", CorrectMode::kLgg},
        {"cdl", CorrectMode::kCdl},
        {"curves", CorrectMode::kCurves},
    };
    return k;
}

std::span<const EnumStrings<KeyMixMode>> keymix_table() {
    static const EnumStrings<KeyMixMode> k[] = {
        {"add", KeyMixMode::kAdd},
        {"subtract", KeyMixMode::kSubtract},
        {"intersect", KeyMixMode::kIntersect},
        {"invert", KeyMixMode::kInvert},
    };
    return k;
}

std::span<const EnumStrings<BlendMode>> blend_table() {
    static const EnumStrings<BlendMode> k[] = {
        {"normal", BlendMode::kNormal},
        {"screen", BlendMode::kScreen},
        {"multiply", BlendMode::kMultiply},
        {"overlay", BlendMode::kOverlay},
        {"soft_light", BlendMode::kSoftLight},
        {"add", BlendMode::kAdd},
        {"subtract", BlendMode::kSubtract},
        {"difference", BlendMode::kDifference},
    };
    return k;
}

template <typename E>
const char* to_string(std::span<const EnumStrings<E>> table, E value, const char* fallback) {
    for (const auto& e : table)
        if (e.value == value) return e.name;
    return fallback;
}

template <typename E>
E from_string(std::span<const EnumStrings<E>> table, const std::string& s, E fallback) {
    for (const auto& e : table)
        if (s == e.name) return e.value;
    return fallback;
}

const char* kind_name(const NodeKind k) { return to_string(kind_table(), k, "corrector"); }
const char* pipe_name(const PipeType t) { return to_string(pipe_table(), t, "rgb"); }
const char* mode_name(const CorrectMode m) { return to_string(mode_table(), m, "identity"); }
const char* keymix_name(const KeyMixMode m) { return to_string(keymix_table(), m, "add"); }
const char* blend_name(const BlendMode b) { return to_string(blend_table(), b, "normal"); }

json pipe_to_json(const PipeId& p) {
    return json{{"node", p.node}, {"type", pipe_name(p.type)}, {"port", p.port}};
}

PipeId pipe_from_json(const json& j) {
    PipeId p;
    p.node = j.value("node", -1);
    p.type = from_string(pipe_table(), j.value("type", std::string("rgb")), PipeType::kRgb);
    p.port = j.value("port", 0);
    return p;
}

json node_to_json(const Node& n) {
    json j{{"id", n.id},
           {"kind", kind_name(n.kind)},
           {"label", n.label},
           {"bypass", n.bypass},
           {"opacity", n.opacity},
           {"partner", n.partner},
           {"shared_source", n.shared_source},
           {"correct_mode", mode_name(n.correct_mode)},
           {"key_mode", keymix_name(n.key_mode)},
           {"blend", blend_name(n.blend)}};
    if (n.lgg) {
        const colorsci::LGG& p = *n.lgg;
        j["lgg"] = json{{"lift_master", p.lift_master},
                        {"gamma_master", p.gamma_master},
                        {"gain_master", p.gain_master},
                        {"lift_r", p.lift_r},
                        {"lift_g", p.lift_g},
                        {"lift_b", p.lift_b},
                        {"gamma_r", p.gamma_r},
                        {"gamma_g", p.gamma_g},
                        {"gamma_b", p.gamma_b},
                        {"gain_r", p.gain_r},
                        {"gain_g", p.gain_g},
                        {"gain_b", p.gain_b}};
    }
    if (n.cdl) {
        const colorsci::Cdl& c = *n.cdl;
        j["cdl"] = json{{"slope_r", c.slope_r},
                        {"slope_g", c.slope_g},
                        {"slope_b", c.slope_b},
                        {"offset_r", c.offset_r},
                        {"offset_g", c.offset_g},
                        {"offset_b", c.offset_b},
                        {"power_r", c.power_r},
                        {"power_g", c.power_g},
                        {"power_b", c.power_b},
                        {"sat", c.sat}};
    }
    if (n.offset) {
        const colorsci::Offset& o = *n.offset;
        j["offset"] = json{{"master", o.master}, {"r", o.r}, {"g", o.g}, {"b", o.b}};
    }
    if (n.curves) {
        const colorsci::CurveParams& cv = *n.curves;
        const auto channel_key = [](colorsci::CurveChannel ch) {
            switch (ch) {
                case colorsci::CurveChannel::kLuma: return "luma";
                case colorsci::CurveChannel::kRed: return "red";
                case colorsci::CurveChannel::kGreen: return "green";
                case colorsci::CurveChannel::kBlue: return "blue";
                case colorsci::CurveChannel::kCount: return "luma";
            }
            return "luma";
        };
        json cp = json::object();
        for (int ci = 0; ci < colorsci::kCurveChannelCount; ++ci) {
            const auto& pts = cv.channels[static_cast<std::size_t>(ci)];
            if (pts.empty()) continue;
            json arr = json::array();
            for (const colorsci::CurvePoint& p : pts) arr.push_back(json{{"x", p.x}, {"y", p.y}});
            cp[channel_key(static_cast<colorsci::CurveChannel>(ci))] = std::move(arr);
        }
        if (!cv.soft_clip.is_identity()) {
            cp["soft_clip"] = json{{"low", cv.soft_clip.low},
                                   {"low_soft", cv.soft_clip.low_soft},
                                   {"high", cv.soft_clip.high},
                                   {"high_soft", cv.soft_clip.high_soft}};
        }
        j["curves"] = std::move(cp);
    }
    return j;
}

}  // namespace

json grade_graph_to_json(const GradeGraph& g) {
    json nodes = json::array();
    for (std::size_t i = 0; i < g.num_nodes(); ++i) nodes.push_back(node_to_json(g.node(static_cast<int>(i))));
    json edges = json::array();
    // Id-order stable so the round trip byte-matches when nothing changed.
    std::vector<const Edge*> by_key;
    by_key.reserve(g.edges().size());
    for (const Edge& e : g.edges()) by_key.push_back(&e);
    std::stable_sort(by_key.begin(), by_key.end(),
                     [](const Edge* a, const Edge* b) {
                         if (a->from.node != b->from.node) return a->from.node < b->from.node;
                         if (a->to.node != b->to.node) return a->to.node < b->to.node;
                         return static_cast<int>(a->from.type) < static_cast<int>(b->from.type);
                     });
    for (const Edge* e : by_key) {
        edges.push_back(json{{"from", pipe_to_json(e->from)}, {"to", pipe_to_json(e->to)}});
    }
    return json{{"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
}

GradeGraph grade_graph_from_json(const json& j) {
    GradeGraph g;
    // Node ids must be re-applied in file order; add_node() assigns ids
    // sequentially (0..N-1), which the writer guarantees by construction.
    std::vector<int> file_to_local;
    for (const auto& nj : j.value("nodes", json::array())) {
        const std::string ks = nj.value("kind", std::string("corrector"));
        bool known = false;
        for (const auto& e : kind_table())
            if (ks == e.name) { known = true; break; }
        if (!known) {
            file_to_local.push_back(-1);  // skip; edges to it are dropped below
            continue;
        }
        const int local = g.add_node(from_string(kind_table(), ks, NodeKind::kCorrector));
        file_to_local.push_back(local);
        Node& n = g.node(local);
        n.label = nj.value("label", n.label);
        n.bypass = nj.value("bypass", n.bypass);
        n.opacity = nj.value("opacity", n.opacity);
        n.partner = nj.value("partner", n.partner);
        n.shared_source = nj.value("shared_source", n.shared_source);
        n.correct_mode =
            from_string(mode_table(), nj.value("correct_mode", std::string("identity")),
                        CorrectMode::kIdentity);
        n.key_mode = from_string(keymix_table(), nj.value("key_mode", std::string("add")),
                                 KeyMixMode::kAdd);
        n.blend =
            from_string(blend_table(), nj.value("blend", std::string("normal")), BlendMode::kNormal);
        if (nj.contains("lgg")) {
            const json& p = nj.at("lgg");
            n.lgg.emplace();
            n.lgg->lift_master = p.value("lift_master", colorsci::LGG{}.lift_master);
            n.lgg->gamma_master = p.value("gamma_master", colorsci::LGG{}.gamma_master);
            n.lgg->gain_master = p.value("gain_master", colorsci::LGG{}.gain_master);
            n.lgg->lift_r = p.value("lift_r", 0.0f);
            n.lgg->lift_g = p.value("lift_g", 0.0f);
            n.lgg->lift_b = p.value("lift_b", 0.0f);
            n.lgg->gamma_r = p.value("gamma_r", 1.0f);
            n.lgg->gamma_g = p.value("gamma_g", 1.0f);
            n.lgg->gamma_b = p.value("gamma_b", 1.0f);
            n.lgg->gain_r = p.value("gain_r", 1.0f);
            n.lgg->gain_g = p.value("gain_g", 1.0f);
            n.lgg->gain_b = p.value("gain_b", 1.0f);
        }
        if (nj.contains("cdl")) {
            const json& c = nj.at("cdl");
            n.cdl.emplace();
            n.cdl->slope_r = c.value("slope_r", 1.0f);
            n.cdl->slope_g = c.value("slope_g", 1.0f);
            n.cdl->slope_b = c.value("slope_b", 1.0f);
            n.cdl->offset_r = c.value("offset_r", 0.0f);
            n.cdl->offset_g = c.value("offset_g", 0.0f);
            n.cdl->offset_b = c.value("offset_b", 0.0f);
            n.cdl->power_r = c.value("power_r", 1.0f);
            n.cdl->power_g = c.value("power_g", 1.0f);
            n.cdl->power_b = c.value("power_b", 1.0f);
            n.cdl->sat = c.value("sat", 1.0f);
        }
        if (nj.contains("offset")) {
            const json& o = nj.at("offset");
            n.offset.emplace();
            n.offset->master = o.value("master", 0.0f);
            n.offset->r = o.value("r", 0.0f);
            n.offset->g = o.value("g", 0.0f);
            n.offset->b = o.value("b", 0.0f);
        }
        if (nj.contains("curves")) {
            const json& cp = nj.at("curves");
            n.curves.emplace();
            const auto parse_channel = [&](const char* key, colorsci::CurveChannel ch) {
                if (!cp.contains(key)) return;
                auto& pts =
                    n.curves->channels[static_cast<std::size_t>(ch)];
                for (const auto& p : cp.at(key)) {
                    pts.push_back(colorsci::CurvePoint{p.value("x", 0.0f), p.value("y", 0.0f)});
                }
            };
            parse_channel("luma", colorsci::CurveChannel::kLuma);
            parse_channel("red", colorsci::CurveChannel::kRed);
            parse_channel("green", colorsci::CurveChannel::kGreen);
            parse_channel("blue", colorsci::CurveChannel::kBlue);
            if (cp.contains("soft_clip")) {
                const json& sc = cp.at("soft_clip");
                n.curves->soft_clip.low = sc.value("low", 0.0f);
                n.curves->soft_clip.low_soft = sc.value("low_soft", 0.0f);
                n.curves->soft_clip.high = sc.value("high", 1.0f);
                n.curves->soft_clip.high_soft = sc.value("high_soft", 0.0f);
            }
        }
    }

    for (const auto& ej : j.value("edges", json::array())) {
        const PipeId from = pipe_from_json(ej.value("from", json::object()));
        const PipeId to = pipe_from_json(ej.value("to", json::object()));
        if (from.node < 0 || to.node < 0) continue;
        const int fl = static_cast<std::size_t>(from.node) < file_to_local.size()
                           ? file_to_local[static_cast<std::size_t>(from.node)]
                           : -1;
        const int tl = static_cast<std::size_t>(to.node) < file_to_local.size()
                           ? file_to_local[static_cast<std::size_t>(to.node)]
                           : -1;
        if (fl < 0 || tl < 0) continue;  // edge touched a skipped node
        if (from.type == PipeType::kChannel) {
            g.add_channel_edge(fl, from.port, tl, to.port);
        } else if (from.type == PipeType::kKey) {
            static_cast<void>(g.add_key_edge(fl, tl));
        } else {
            static_cast<void>(g.add_rgb_edge(fl, tl));
        }
    }
    return g;
}

}  // namespace canvas::core::grade_graph