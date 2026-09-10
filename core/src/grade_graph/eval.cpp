#include "canvas/core/grade_graph/eval.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>

namespace canvas::core::grade_graph {

FrameF FrameF::filled(int w, int h, float r, float g, float b, float a) {
    FrameF f;
    f.w = w;
    f.h = h;
    f.rgba.assign(static_cast<std::size_t>(w * h) * 4u, 0.0f);
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
        f.rgba[i] = r;
        f.rgba[i + 1] = g;
        f.rgba[i + 2] = b;
        f.rgba[i + 3] = a;
    }
    return f;
}

float* FrameF::at(int x, int y) {
    return &rgba[static_cast<std::size_t>(y * w + x) * 4u];
}

const float* FrameF::at(int x, int y) const {
    return &rgba[static_cast<std::size_t>(y * w + x) * 4u];
}

GrayF GrayF::filled(int w, int h, float value) {
    GrayF g;
    g.w = w;
    g.h = h;
    g.v.assign(static_cast<std::size_t>(w * h), value);
    return g;
}

namespace {

// ---- per-node correction (the spec's "corrected" operand) -----------------

colorsci::RGBF apply_correction(const colorsci::RGBF& p, const Node& node) {
    switch (node.correct_mode) {
        case CorrectMode::kLgg: {
            colorsci::RGBF out = p;
            if (node.offset) out = colorsci::apply_offset(out, *node.offset);
            return colorsci::apply_lgg(out, node.lgg.value_or(colorsci::LGG{}));
        }
        case CorrectMode::kCdl:
            return colorsci::apply_cdl(p, node.cdl.value_or(colorsci::Cdl{}));
        case CorrectMode::kCurves:
            return colorsci::apply_curves(p, node.curves.value_or(colorsci::CurveParams{}));
        case CorrectMode::kIdentity:
            break;
    }
    return p;
}

std::shared_ptr<FrameF> corrected_image(const FrameF& in, const Node& node) {
    auto out = std::make_shared<FrameF>(FrameF::filled(in.w, in.h, 0.0f, 0.0f, 0.0f));
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            const float* src = in.at(x, y);
            float* dst = out->at(x, y);
            const colorsci::RGBF res = apply_correction({src[0], src[1], src[2]}, node);
            dst[0] = res.r;
            dst[1] = res.g;
            dst[2] = res.b;
            dst[3] = src[3];
        }
    }
    return out;
}

std::shared_ptr<GrayF> ones_like(const FrameF& in) {
    return std::make_shared<GrayF>(GrayF::filled(in.w, in.h, 1.0f));
}

// mix(in, corrected, key * opacity) written into out. Key and frame sizes
// always match (uniform keys are widened to the frame size at the seam).
void blend_by_key(const FrameF& in, const FrameF& corrected, const GrayF& key,
                  float opacity, FrameF& out) {
    const std::size_t pixels = static_cast<std::size_t>(in.w) * in.h;
    for (std::size_t i = 0; i < pixels; ++i) {
        const float eff = key.v[i] * opacity;
        const float* a = &in.rgba[i * 4];
        const float* b = &corrected.rgba[i * 4];
        float* o = &out.rgba[i * 4];
        o[0] = a[0] + (b[0] - a[0]) * eff;
        o[1] = a[1] + (b[1] - a[1]) * eff;
        o[2] = a[2] + (b[2] - a[2]) * eff;
        o[3] = a[3];
    }
}

// ---- graph plumbing --------------------------------------------------------

std::vector<std::pair<int, int>> key_sources_of(const GradeGraph& g, int node) {
    std::vector<std::pair<int, int>> out;
    for (const Edge& e : g.edges()) {
        if (e.to.node == node && e.to.type == PipeType::kKey) out.emplace_back(e.to.port, e.from.node);
    }
    return out;
}

std::vector<std::pair<int, int>> rgb_sources_of(const GradeGraph& g, int node) {
    std::vector<std::pair<int, int>> out;
    for (const Edge& e : g.edges()) {
        if (e.to.node == node && e.to.type == PipeType::kRgb) out.emplace_back(e.to.port, e.from.node);
    }
    return out;
}

struct EvalState {
    GradeGraph graph;
    FrameF source;
    std::vector<bool> active;
    std::vector<std::shared_ptr<const FrameF>> rgb;
    std::vector<std::shared_ptr<const GrayF>> key;
    std::vector<std::array<std::shared_ptr<const GrayF>, 3>> channel;

    const FrameF* input_rgb(int node) const {
        const std::optional<int> src = graph.rgb_source_of(node);
        if (src && *src >= 0 && static_cast<std::size_t>(*src) < rgb.size() && rgb[*src]) {
            return rgb[*src].get();
        }
        return &source;
    }

    // The effective key input of a node: the frame-sized key_out of whatever
    // provides its port-0 key edge, or a uniform 1.0 when unconnected.
    std::shared_ptr<const GrayF> key_for(int node) const {
        const std::vector<std::pair<int, int>> ks = key_sources_of(graph, node);
        for (const auto& [port, src] : ks) {
            if (port == 0 && src >= 0 && static_cast<std::size_t>(src) < key.size() && key[src]) {
                return key[src];
            }
        }
        return std::make_shared<GrayF>(GrayF::filled(source.w, source.h, 1.0f));
    }
};

}  // namespace

void blend_into(const float* acc, const float* layer, float* out, std::size_t n,
                BlendMode blend) {
    for (std::size_t i = 0; i < n; ++i) {
        const float* a = &acc[i * 4];
        const float* l = &layer[i * 4];
        float* o = &out[i * 4];
        for (int c = 0; c < 3; ++c) {
            const float A = a[c];
            const float B = l[c];
            switch (blend) {
                case BlendMode::kScreen:
                    o[c] = 1.0f - (1.0f - A) * (1.0f - B);
                    break;
                case BlendMode::kMultiply:
                    o[c] = A * B;
                    break;
                case BlendMode::kOverlay:
                    o[c] = A <= 0.5f ? 2.0f * A * B : 1.0f - 2.0f * (1.0f - A) * (1.0f - B);
                    break;
                case BlendMode::kSoftLight:
                    o[c] = B <= 0.5f ? A - (1.0f - 2.0f * B) * A * (1.0f - A)
                                     : A + (2.0f * B - 1.0f) *
                                               (A <= 0.25f ? ((16.0f * A - 12.0f) * A + 4.0f) * A
                                                           : std::sqrt(A) - A);
                    break;
                case BlendMode::kAdd:
                    o[c] = A + B;
                    break;
                case BlendMode::kSubtract:
                    o[c] = A - B;
                    break;
                case BlendMode::kDifference:
                    o[c] = std::fabs(A - B);
                    break;
                case BlendMode::kNormal:
                default:
                    o[c] = B;
                    break;
            }
        }
        o[3] = a[3];
    }
}

namespace {

std::shared_ptr<GrayF> outside_key_of(const EvalState& st, const Node& n) {
    // key_out = 1 - partner.key_out (live, auto-updating). A missing partner
    // defaults to ones -> the outside gets a zero key and stays silent.
    std::shared_ptr<GrayF> pk =
        (n.partner >= 0 && static_cast<std::size_t>(n.partner) < st.key.size() &&
         st.key[n.partner])
            ? std::make_shared<GrayF>(*st.key[n.partner])
            : ones_like(st.source);
    for (float& v : pk->v) v = 1.0f - v;
    return pk;
}

// The key that gates a node's own correction. Outside nodes ignore their key
// input entirely — their partition IS the inverted partner key, per spec.
std::shared_ptr<const GrayF> blend_key_for(EvalState& st, const Node& n) {
    if (n.kind == NodeKind::kOutside) return outside_key_of(st, n);
    return st.key_for(n.id);
}

std::shared_ptr<const FrameF> eval_corrector(EvalState& st, const Node& n) {
    const FrameF* in = st.input_rgb(n.id);
    if (n.bypass) return std::make_shared<FrameF>(*in);
    const std::shared_ptr<FrameF> corrected = corrected_image(*in, n);
    // Layer nodes hand the full correction to their Layer Mixer, which owns
    // the single key*opacity gate. Pre-blending here would square the effect.
    if (n.kind == NodeKind::kLayer) return corrected;
    const std::shared_ptr<const GrayF> k = blend_key_for(st, n);
    auto out = std::make_shared<FrameF>(FrameF::filled(st.source.w, st.source.h, 0.0f, 0.0f, 0.0f));
    blend_by_key(*in, *corrected, *k, n.opacity, *out);
    return out;
}

std::shared_ptr<const GrayF> eval_key(EvalState& st, const Node& n) {
    switch (n.kind) {
        case NodeKind::kCorrector:
            // Serial nodes pass their key through (feathered upstream mattes
            // keep propagating downstream).
            return st.key_for(n.id);
        case NodeKind::kOutside:
            return outside_key_of(st, n);
        case NodeKind::kKeyMixer: {
            const std::vector<std::pair<int, int>> ks = key_sources_of(st.graph, n.id);
            std::shared_ptr<GrayF> A = ones_like(st.source);
            std::shared_ptr<GrayF> B = ones_like(st.source);
            for (const auto& [port, src] : ks) {
                if (src >= 0 && static_cast<std::size_t>(src) < st.key.size() && st.key[src]) {
                    if (port == 0) A = std::make_shared<GrayF>(*st.key[src]);
                    else B = std::make_shared<GrayF>(*st.key[src]);
                }
            }
            auto out = std::make_shared<GrayF>(GrayF::filled(st.source.w, st.source.h, 0.0f));
            const std::size_t npx = static_cast<std::size_t>(st.source.w) * st.source.h;
            for (std::size_t i = 0; i < npx; ++i) {
                const float a = A->v[i];
                const float b = B->v[i];
                switch (n.key_mode) {
                    case KeyMixMode::kAdd:
                        out->v[i] = std::clamp(a + b, 0.0f, 1.0f);
                        break;
                    case KeyMixMode::kSubtract:
                        out->v[i] = std::clamp(a - b, 0.0f, 1.0f);
                        break;
                    case KeyMixMode::kIntersect:
                        out->v[i] = a * b;
                        break;
                    case KeyMixMode::kInvert:
                        out->v[i] = 1.0f - a;
                        break;
                }
            }
            return out;
        }
        default:
            // Parallel/Layer nodes use their own key only; they don't propagate.
            return ones_like(st.source);
    }
}

std::shared_ptr<const FrameF> eval_node(EvalState& st, const Node& n) {
    switch (n.kind) {
        case NodeKind::kCorrector:
        case NodeKind::kParallel:
        case NodeKind::kLayer:
        case NodeKind::kOutside:
            return eval_corrector(st, n);
        case NodeKind::kParallelMixer: {
            const std::vector<std::pair<int, int>> ins = rgb_sources_of(st.graph, n.id);
            const FrameF* A = ins.size() > 0 && ins[0].second >= 0 ? st.rgb[ins[0].second].get() : nullptr;
            const FrameF* B = ins.size() > 1 && ins[1].second >= 0 ? st.rgb[ins[1].second].get() : nullptr;
            if (!A || !B) return nullptr;
            const FrameF* base = n.shared_source >= 0 ? st.rgb[n.shared_source].get() : &st.source;
            auto out = std::make_shared<FrameF>(FrameF::filled(st.source.w, st.source.h, 0.0f, 0.0f, 0.0f));
            const std::size_t pixels = static_cast<std::size_t>(st.source.w) * st.source.h;
            for (std::size_t i = 0; i < pixels; ++i) {
                for (int c = 0; c < 3; ++c) {
                    out->rgba[i * 4 + c] = A->rgba[i * 4 + c] + B->rgba[i * 4 + c] -
                                           base->rgba[i * 4 + c];
                }
                out->rgba[i * 4 + 3] = base->rgba[i * 4 + 3];
            }
            return out;
        }
        case NodeKind::kLayerMixer: {
            const std::vector<std::pair<int, int>> ins = rgb_sources_of(st.graph, n.id);
            if (ins.empty()) return nullptr;
            const FrameF* base = ins[0].second >= 0 ? st.rgb[ins[0].second].get() : &st.source;
            if (!base) return nullptr;
            auto acc = std::make_shared<FrameF>(*base);
            auto tmp = std::make_shared<FrameF>(*base);
            const std::size_t npx = static_cast<std::size_t>(st.source.w) * st.source.h;
            // Remaining inputs, in port order, composite bottom-to-top.
            std::vector<std::pair<int, int>> layers(ins.begin() + 1, ins.end());
            std::stable_sort(layers.begin(), layers.end(),
                             [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                                 return a.first < b.first;
                             });
            for (const auto& [port, src] : layers) {
                (void)port;
                if (src < 0) continue;
                const std::shared_ptr<const FrameF> layer = st.rgb[src];
                if (!layer) continue;
                blend_into(acc->rgba.data(), layer->rgba.data(), tmp->rgba.data(), npx,
                           st.graph.node(src).blend);
                const std::shared_ptr<const GrayF> k = st.key_for(src);
                const float op = st.graph.node(src).opacity;
                for (std::size_t i = 0; i < npx; ++i) {
                    const float eff = k->v[i] * op;
                    for (int c = 0; c < 3; ++c) {
                        acc->rgba[i * 4 + c] += (tmp->rgba[i * 4 + c] - acc->rgba[i * 4 + c]) * eff;
                    }
                }
            }
            return acc;
        }
        case NodeKind::kSplitter: {
            const FrameF* in = st.input_rgb(n.id);
            for (int p = 0; p < 3; ++p) {
                auto g = std::make_shared<GrayF>(GrayF::filled(st.source.w, st.source.h, 0.0f));
                for (std::size_t i = 0; i < g->v.size(); ++i) g->v[i] = in->rgba[i * 4 + p];
                st.channel[n.id][p] = g;
            }
            return nullptr;  // no rgb out; channel outs only
        }
        case NodeKind::kCombiner: {
            auto out = std::make_shared<FrameF>(FrameF::filled(st.source.w, st.source.h, 0.0f, 0.0f, 0.0f));
            for (const auto& [port, src] : st.graph.channel_sources_of(n.id)) {
                if (src >= 0 && static_cast<std::size_t>(src) < st.channel.size() &&
                    st.channel[src][port]) {
                    for (std::size_t i = 0; i < st.channel[src][port]->v.size(); ++i) {
                        out->rgba[i * 4 + port] = st.channel[src][port]->v[i];
                    }
                }
            }
            return out;
        }
        case NodeKind::kOutput: {
            const FrameF* in = st.input_rgb(n.id);
            return in ? std::make_shared<FrameF>(*in) : nullptr;
        }
        case NodeKind::kKeyMixer:
            return nullptr;  // key-only node
    }
    return nullptr;
}

}  // namespace

EvalResult evaluate_graph(const GradeGraph& g, const FrameF& source) {
    EvalResult result;
    const int terminal = g.terminal();
    if (terminal < 0) {
        // No output terminal wired -> the whole tree is inactive; passthrough.
        result.frame = nullptr;
        return result;
    }

    const std::size_t n = g.num_nodes();
    EvalState st;
    st.graph = g;
    st.source = source;
    st.active.assign(n, false);
    st.rgb.assign(n, {});
    st.key.assign(n, {});
    st.channel.assign(n, {});

    std::vector<std::vector<int>> rev(n);
    std::vector<std::vector<int>> fwd(n);
    auto add_dep = [&](int from, int to) {
        if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= n ||
            static_cast<std::size_t>(to) >= n)
            return;
        fwd[from].push_back(to);
        rev[to].push_back(from);
    };
    for (const Edge& e : g.edges()) add_dep(e.from.node, e.to.node);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& nd = g.node(static_cast<int>(i));
        if (nd.kind == NodeKind::kOutside) add_dep(nd.partner, nd.id);
        if (nd.kind == NodeKind::kParallelMixer) add_dep(nd.shared_source, nd.id);
    }

    // Reachability from the terminal (reverse edges) decides the active set.
    std::vector<int> stack{terminal};
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        if (cur < 0 || static_cast<std::size_t>(cur) >= n || st.active[cur]) continue;
        st.active[cur] = true;
        for (const int p : rev[static_cast<std::size_t>(cur)]) stack.push_back(p);
    }

    // Kahn topological order over the active subgraph (both pipes + implicits).
    std::vector<int> indeg(n, 0);
    for (std::size_t u = 0; u < n; ++u) {
        if (!st.active[u]) continue;
        for (const int v : fwd[u]) {
            if (st.active[v]) ++indeg[v];
        }
    }
    std::queue<int> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (st.active[i] && indeg[i] == 0) ready.push(static_cast<int>(i));
    }
    std::vector<int> order;
    order.reserve(n);
    while (!ready.empty()) {
        const int u = ready.front();
        ready.pop();
        order.push_back(u);
        for (const int v : fwd[u]) {
            if (!st.active[v]) continue;
            if (--indeg[v] == 0) ready.push(v);
        }
    }
    std::size_t active_count = 0;
    for (const bool a : st.active) active_count += a ? 1u : 0u;
    if (order.size() != active_count) {
        result.error = "grade graph contains a cycle";
        result.frame = nullptr;
        return result;
    }

    for (const int id : order) {
        const Node& nd = g.node(id);
        const std::shared_ptr<const FrameF> rgb = eval_node(st, nd);
        if (rgb) st.rgb[id] = rgb;
        if (nd.kind == NodeKind::kCorrector || nd.kind == NodeKind::kOutside ||
            nd.kind == NodeKind::kKeyMixer) {
            st.key[id] = eval_key(st, nd);
        }
    }

    result.frame = st.rgb[static_cast<std::size_t>(terminal)];
    if (!result.frame) result.error = "grade graph terminal produced no frame";
    return result;
}

}  // namespace canvas::core::grade_graph