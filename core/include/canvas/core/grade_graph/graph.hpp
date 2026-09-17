#pragma once

#include "canvas/core/colorsci/cdl.hpp"
#include "canvas/core/colorsci/curves.hpp"
#include "canvas/core/colorsci/wheels.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace canvas::core::grade_graph {

enum class PipeType { kRgb, kKey, kChannel };

enum class NodeKind {
    kCorrector,
    kParallel,
    kLayer,
    kOutside,
    kKeyMixer,
    kSplitter,
    kCombiner,
    kParallelMixer,
    kLayerMixer,
    kOutput,
};

enum class OpKind { kIdentity, kLgg, kCdl, kCurves };
using CorrectMode = OpKind;

enum class KeyMixMode { kAdd, kSubtract, kIntersect, kInvert };

enum class CompositeOp {
    kOver,
    kIn,
    kOut,
    kAtop,
    kXor,
    kDisjoint,
    kMask,
    kStencil,
};

enum class BlendMode {
    kNormal,
    kScreen,
    kMultiply,
    kOverlay,
    kSoftLight,
    kAdd,
    kSubtract,
    kDifference,
};

struct Node {
    int id = 0;
    NodeKind kind = NodeKind::kCorrector;
    std::string label;
    bool bypass = false;
    float opacity = 1.0f;

    int partner = -1;
    int shared_source = -1;

    OpKind correct_mode = OpKind::kIdentity;
    std::optional<colorsci::LGG> lgg;
    std::optional<colorsci::Cdl> cdl;
    std::optional<colorsci::Offset> offset;
    std::optional<colorsci::CurveParams> curves;

    KeyMixMode key_mode = KeyMixMode::kAdd;
    BlendMode blend = BlendMode::kNormal;
    CompositeOp composite_op = CompositeOp::kOver;
    float additive = 0.0f;
};

struct PipeId {
    int node = -1;
    PipeType type = PipeType::kRgb;
    int port = 0;
};

struct Edge {
    PipeId from;
    PipeId to;
};

class GradeGraph {
public:
    int add_node(NodeKind kind);
    [[nodiscard]] Node& node(int id);
    [[nodiscard]] const Node& node(int id) const;

    [[nodiscard]] int add_edge(PipeId from, PipeId to);
    int add_rgb_edge(int from, int to);
    int add_key_edge(int from, int to);
    int add_channel_edge(int from, int port, int to, int port2);
    [[nodiscard]] bool remove_edge(PipeId from, PipeId to);
    [[nodiscard]] bool would_create_cycle(PipeId from, PipeId to) const;

    [[nodiscard]] int terminal() const;
    [[nodiscard]] std::optional<int> rgb_source_of(int node) const;
    [[nodiscard]] std::vector<std::pair<int, int>> key_sources_of(int node) const;
    [[nodiscard]] std::vector<std::pair<int, int>> channel_sources_of(int node) const;

    [[nodiscard]] std::size_t num_nodes() const;
    [[nodiscard]] const std::vector<Edge>& edges() const;
    void clear();

    std::uint64_t change_seq = 0;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

}