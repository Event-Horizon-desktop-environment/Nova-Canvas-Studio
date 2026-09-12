# Node System Research — Effects, Layers & Color Grading for Nova

> Research doc. Compiled 2026-09-11 from DaVinci Resolve 18/19/21 Reference Manuals + the Fusion 9/17
> manuals (official Blackmagic documents.blackmagicdesign.com), OpenFX 1.5.1 reference documentation
> (AcademySoftwareFoundation), Blender 5.2 compositor documentation, The Foundry Nuke material,
> FilmLight Baselight datasheets/BLG tooling, Adobe After Effects documentation, and practitioner
> sources cross-checked against the official manuals. Companion to `docs/color.md` (color science +
> the Resolve Color page) and the repo's own `grade_graph` implementation
> (`core/include/canvas/core/grade_graph/`).
>
> Question this doc answers: **what node system should Nova build so that color grading, effects,
> and layering/compositing are governed by one coherent law instead of three bolt-ons?** The answer
> has two parts: a **model** decision (one graph, one pipe set, pluggable op kinds) and a **law**
> decision (a single compositing/algebra core for layers + keys, with effect ops as parameterized
> members of the same family). Where the industry evidence is unambiguous, this doc says so; where
> systems disagree, it records both and picks based on Nova's constraints (headless, zero-warning,
> GPU later, Resolve-parity UI).

Note on method: a live probe of the installed DaVinci Resolve's node graph (via the Resolve MCP
`probe_node_graph`/`color_group` surface) was attempted but the MCP daemon's tools were not
exposed to this session, so the Resolve semantics below are read from the official manuals rather
than measured. The manual text is quoted where it decides a design point.

---

## Part I — The convergence: one graph, many op families

Every professional system that has *both* color work and effects/compositing ends up as **one
node graph whose nodes are interchangeable operations** — the "color node" and the "effect node"
are the same node shape with a different parameterization. The evidence:

| System | Graph model | Effects + color in one graph? | Layering carried by |
|---|---|---|---|
| **Resolve Color** | Node canvas (serial/parallel/layer/outside/mixers + key pipe) | **Yes** — OpenFX/Resolve FX are *dropped onto color nodes* (`documents.blackmagicdesign.com` Resolve manuals, "Using Open FX and Resolve FX") | The **Layer Mixer** node + per-layer composite modes; higher/lower priority by input slot |
| **Fusion (Resolve's engine)** | Node tree ("each node is its own image processing operation… nodes are connected together to create a node tree") | **Yes** — over 200 tools: color, blur/glow/grain, keyers, all in one flow | The **Merge / MultiMerge** node — Porter–Duff algebra over premultiplied images |
| **Nuke** | Node DAG, multichannel scanline engine, 32-bit float | **Yes** — `Grade`, `Blur`, `Merge` are peers | **Merge** (A-over-B with channels), Premult/Unpremult discipline |
| **Baselight** | Layer stack UI over the node tree it represents; Photoshop-style blend modes between layers | **Yes** — and it is literally a *Nuke node*: the **BLG** exports the full grade stack as a "multi-input node in NUKE" so Baselight layers composite inside a Nuke graph | The grade **layer stack** (primary overall or matte-restricted secondary), blend modes, "implicit alpha handling" |
| **After Effects** | Layer-based (bottom rendered first, top on top); per-layer fixed order **masks → effects → transforms → styles**; adjustment layers affect everything below | **Partial** — effects live *inside* a layer, not as graph nodes; the graph is the layer stack | The layer stack itself + blend/transfer modes |

The decisive existence proofs:

- **Baselight-as-Nuke-node.** "Baselight can also act as a multi-input node in NUKE so that BLG files
  can refer to multiple input images… the BLG also stores format and mapping information" — a whole
  grade with all its layers and mattes is *one node* in a compositing graph. If the industry's
  pure-color tool renders as a node, Nova's grade graph should too: **the per-clip grade tree is
  itself a node kind** in a larger tree (its `kOutput` terminal becomes an input port to the outer
  graph). This confirms the unified direction and rules out two separate systems.
- **Resolve hosts OFX effect nodes inside the color node tree.** The "effects" and the "grade" share
  one pipeline per clip. There is no second, separate effect engine.

Rule for Nova: **one node graph, one set of pipes; node *kinds* partition by topology (chain,
branch, merge, key, terminal), and a pluggable *op* slot parameterizes what each node computes.**
The existing `grade_graph` already has the right topology kinds and the right pipe set; it just
hardcodes one op family (`correct_mode ∈ {identity, lgg, cdl, curves}`) on corrector-shaped nodes.
That is the seam the rest of this doc widens.

---

## Part II — The data model (pipes, alpha, values)

### 2.1 Pipes — Nova's three already match the industry

Fusion's Merge has *background + foreground + effect-mask*; Resolve nodes have a green RGB and a
blue key connector; Nuke has image channels plus aux (z, normals); OFX clips are RGBA/RGB/Alpha
components with an optional **mask clip** (`kOfxImageClipPropIsMask`). Nova's `PipeType
{kRgb, kKey, kChannel}` in `graph.hpp` is the right shape: an image pipe, a single-channel matte
pipe, and a channel pipe.

Two generalizations are worth *recording* but should be deferred (see Part V):

- **Value/scalar wires** (Blender): "Compositing nodes operate on data that is either an image or a
  dimensionless single value… if image, input is per-pixel; if single value, it covers the whole
  space." This is how Blender lets one node's single-valued output (e.g. a Levels measurement or a
  time parameter) drive another node's input *as a wire*. Nova v1 keeps op parameters as node data
  (the current `LGG`/`CurveParams` style); scalar wires are a clean later extension because the
  port table (`node_ports`) already centralizes arity.
- **Multichannel / EXR passes** (Nuke, OpenEXR): defer; the alpha-discipline below matters first.

### 2.2 Alpha discipline — premultiplied interior, straight at the boundary

This is the single biggest *correctness* gap between the current Nova evaluator and any real
compositing law.

- Current `blend_into` (`core/src/grade_graph/eval.cpp:147`) blends **straight RGB** with a
  `key*opacity` scalar: `out = acc + (layered - acc) * eff`, and copies alpha through untouched.
  That is a correct *single-source keyed blend* (right for "apply this node's correction gated by a
  matte"), but it is **not a compositing law** — it cannot represent A-over-B with two real alpha
  channels, `In`/`Out`/`Atop`/`XOR`, or premultiplied edges.
- Fusion is explicit: the Merge node "combines two images based on the Alpha (opacity) channel,…
  supports the standard Over, In, Held Out, Atop, and XOr methods" and can run **additive
  (premultiplied) or subtractive (non-premultiplied)** compositing, with an Additive↔Subtractive
  slider for problem edges. Gaffer's `Merge` documents the same underlying op set as *alpha
  equations* (`Over: A + B(1-a)`, `In: Ab`, `Out: A(1-b)`, `Atop`, `XOr`, `Matte`, `Mask`,
  arithmetic…). AE likewise composites on alpha and lets "render transformation before effects".

Recommendation — two compositing fundamentals in one headless law module (`composite.hpp` in
`core/include/canvas/core/grade_graph/`):

1. A **Porter–Duff operator set** as `CompositeOp { kOver, kIn, kOut, kAtop, kXor, kDisjoint,
   kMask, kStencil }` with the standard alpha coefficients (Fusion names), applied on
   **premultiplied** RGB; alpha always recomputed by the coefficients (Fusion also has the correct
   `Disjoint` alpha combination — "not get out of range Alpha, and premultiplied edges get the
   correct Alpha combination").
2. An **Additive↔Subtractive knob** (Fusion's slider) blending between premultiplied ("additive")
   and straight ("subtractive") handling, so problem edges round-trip.

The existing Photoshop blend family (`kNormal/kScreen/kMultiply/kOverlay/kSoftLight/kAdd/…` in
`graph.hpp`) is a *third* family — replace the `kNormal` straight-lerp with a true Over, keep the
rest as blend-family ops applied *on the premultiplied result*, then gate by the node's key and
opacity exactly as today. The layer-mixer law becomes: composite each layer over the accumulated
stack with `(keys, opacity, blend-family)` driving a per-pixel mix of the Porter–Duff result.

### 2.3 Livesthe `effect mask` already exists — it is the key pipe

OFX's "EffectMask" input ("a clip that is intended to be used as a mask input… where the mask is
zero the effect should not occur, where it is whitepoint the effect should be full-on") is exactly
Nova's key pipe, and the current evaluator already gates `correction * key * opacity`
(`blend_by_key`, eval.cpp:83). So **every node — including future effect ops — gets an effect mask
for free.** A noise/glow/grain op constrained to a region is the "windowed effect" pattern without
a single new mechanism. Windows/qualifiers/mag-masks are simply *key-producing* ops (they write the
blue pipe), matching Resolve ("blue connectors carry key/matte channels").

---

## Part III — Layers: the two shapes both reduce to one law

There are two "layers" concepts in the industry UI, and both should reduce to the same Nova law:

1. **A sub-stack of blended corrections** (Resolve Layer Mixer; Baselight layer stack; AE layer
   stack). Resolve's manual pins the priority convention precisely:

   > "The Layer Mixer prioritizes nodes connected to lower inputs such that each node's output
   > completely obscures whatever is behind it." … "The Layer Mixer node combines the outputs of
   > multiple nodes such that the image output by lower nodes takes priority over images output by
   > higher nodes." (Resolve 18.6 Reference Manual, "Layer Mixer Node Structures")
   > "ADD LAYER – adds a Corrector node as a layer underneath the currently selected node… so that
   > its output has higher mix priority than any other nodes previously connected" (Advanced Panel
   > manual).

   So **the lowest input slot = the topmost layer**. Nova's existing laws already implement the
   *shoulders* of this correctly — `add_layer` appends at the highest port (`highest+1`), i.e. the
   new layer ends up topmost/dominant, exactly "add layer with higher mix priority" — while the
   evaluator composites ascending ports bottom→top (highest port on top). **Conventions are
   compatible** provided the external layer list is pinned as *descending port = top-to-bottom
   priority*. Nova's `set_layer_order`/`add_layer` (already headless-tested) become the law for
   that ordering; the UI layer list renders top = highest port.

   The one wording hazard: Basset the doc `color-grading-phases.md` Phase 2 note says "lowest mixer
   input = topmost layer" (echoing the manual) while the code's `set_layer_order` takes a
   "bottom-to-top" list. Both are true only if "bottom" means the *stack's* bottom (= top of output
   priority). The research doc pins one invariant: **layer list reads top-to-bottom in output
   priority; port numbers ascend bottom→top internally; `set_layer_order`'s argument order === port
   order.** (A one-line comment in `edit.hpp` will make it unambiguous when Phase 6 lands.)

2. **A generic A-over-B merge** (Fusion `Merge`, Nuke `Merge`, Gaffer). The Fusion ops table is the
   reference: `Over, In, Held Out, Atop, XOr, Disjoint, Mask, Stencil` with alpha equations, plus
   `Add/Subtract/Multiply/Screen/Difference/Min/Max` arithmetic. Fusion's `MultiMerge` is the
   N-input version (a background + N foreground layers in a keyframeable layer list), which is
   geometrically the same thing as Nova's Layer Mixer once the alpha law is fixed.

Recommendation: **do not build a separate Merge node kind.** Rule-by-position: the Layer Mixer
*is* the merge — N inputs, base first (edge order contract already enforced by the Phase 6 laws),
layers after, each carrying (blend family, key, opacity, additive/subtractive knob). A dedicated
2-input `kMerge` node becomes an optional *view alias* later if a Fusion-style flow is wanted. The
law module is one: `layer_mix(base, layers…)`.

Resolve-style **parallel mixing** stays as-is (`A + B − base`, "parallel nodes mix their
adjustments equally") — it is a *math* combine, not a compositing one, and the current evaluator +
`insert_branch` law already pin it.

---

## Part IV — Effects: the op taxonomy and the OFX lesson

### 4.1 Op families

Effects are not a new graph engine, they are new members of one op table. Each image node gets an
`op` slot from an extensible `OpKind`:

| Family | Members (Resolve/Fusion-resident names) | Implementation trait |
|---|---|---|
| **Grade** (exists) | LGG/offset, CDL, curves, balance, RGB mixer, HSL curves | pointwise |
| **Transform** | CST/LUT/color-managed nodes (color.md Part II.7), tone/gamut mapping | pointwise; needs pipeline-place |
| **Spatial** | Blur (gaussian/box), Sharpen (unsharp → the reserved `Mid/Detail`), Glow/Halation, Film Grain (spatial/temporal), Light Rays, Bloom, Tilt/Defocus, Vignette, Chromatic Aberration, Soften/mist | **neighborhood** — needs a kernel; GPU pass is the expensive neighbor and the caching trigger |
| **Key** (mask/matte producers) | Qualifier, power windows (circle/poly/linear/gradient), outside-node partner | produce key pipe |
| **Merge/layer** | Layer Mixer (Part III) | compositing law |
| **Generator** (later) | noise/texture, solids (OFX `Generator` context) | no rgb input required |

`bypass`, `opacity`, label are already on `Node`; Resolve has the same ("bypass node selection",
per-node opacity, "node cache" states) — no model change.

### 4.2 The OFX lessons to adopt as concepts (not the ABI)

OpenFX 1.5.1 defines the industry effect contract. Nova's internal op registry should mirror its
concepts so a future OFX host maps 1:1:

- **Contexts as arity/behavior classes**: `Filter` (one input), `General` (arbitrary inputs, tree
  compositing), `Transition` (two inputs + progress), `Retimer` (speed against a source-time
  parameter), `Generator` (no input). Nova v1 needs Filter + General (+ Retimer = existing per-clip
  speed law, outside the color graph).
- **`GetFramesNeeded`** — an op declares which input frames it needs to produce one output frame
  (temporal reach). Grain seeding, temporal blur, and any future temporal op hang on this; giving
  the evaluator a frame/time context and an op-declared reach is the v1 mechanism (see Part V.2).
- **`IsIdentity`** — the host can skip an op whose current params are identity (copy input).
  Nova should implement this as op metadata (`op_is_identity(op, params)`) — free wins for the
  common "reset" case and for `kIdentity`.
- **Optional input clips** — a node may present but not require an input (unconnected = default).
  Nova already models "unconnected rgb = clip source, unconnected key = 1.0"; OFX gives the
  language for "optional" semantics per port.
- **Region-of-definition / "given a region I want to render, what region do you need from input"** —
  the spatial budgeting primitive. v1 can ignore it (full-frame ops); the op registry should leave
  a noted seam for it.

Deliberately **not** adopting the OFX C ABI in v1 matches `color.md` §3.7 (own plugins first; OFX
hosting later). The op registry (`op_id`, param schema, `apply(pixels|frame, params, key, time)`,
CPU reference + GPU shader twin, identity test) IS the plugin contract; an OFX adapter later wraps
the registry.

### 4.3 Effect-specific math notes (recorded, not yet encoded)

- **Blur**: separable; gaussian radius in pixels, kRadius clamp per `visual.hpp` culture.
- **Sharpen / Mid/Detail**: unsharp; already deferred to the Phase 7 GPU pass in
  `color-grading-phases.md` ("Mid/Detail (spatial unsharp)… no neighborhood math here").
- **Glow**: threshold/blur/screen classic; Resolve's Glow/Halation family.
- **Film grain**: per-channel noise with a **deterministic per-frame seed** (time param) so the
  cache is scrub-correct — same seed law as any temp-locked op.
- **Light rays**: directional blur toward a light centroid + screen; Fusion's vector-based lens
  flare/rays tools are the reference.
- **Defocus / Tilt**: cheap anisotropic/fake bokeh in v1 (true DOF from depth later — Nuke's
  `ZDefocus` needs a depth channel, which is the multichannel extension, deferred).

---

## Part V — Evaluation, caching, scope

### 5.1 Scope hierarchy (where graphs hang)

Resolve's order of operations (color.md §2.1): timeline → group pre-clip → clip → group post-clip.
Nova's existing single *per-clip* tree is the middle of that. Recommended minimum:

1. **Clip-level tree** (exists) — the per-shot grade + effects; the `kOutput` becomes an input
   port to the outer graph (Part I).
2. **Sequence/timeline tree** (new) — global looks, output CST/tone-map, and shot-proof whole-film
   grain; its output is the final composite. Placement is pinned by the already-planned render
   `PipelineStage` enum (`pipeline.hpp`, color.md §3.4: input transform → grade → tone map →
   output gamut map → display).

Group pre/post and per-track graphs are v2; the two-level model is the stable Resolve-normal for an
NLE (input CST at group/timeline, creative at clip).

### 5.2 Evaluation law: push reference + cached demand

Nova's evaluator is a clean per-frame **push** (topological Kahn over the terminal-reachable
subgraph, `eval.cpp`). That is the correct reference law and should stay — headless tests pin it.
Industry practice (Nuke's per-node caches + DiskCache node, Resolve's "cache expensive nodes" for
Magic Mask, Blender's per-node caching) adds a **memoization cache on top, never instead of**:

- Per-node cache entry keyed by `(graph change_seq, node params version, frame, resolution)` with
  **lazy invalidation**: editing node N marks only its descendants dirty; an unchanged upstream is
  re-served from cache. This is the difference between interactive scrubbing with a 2-node versus
  40-node tree, and it layers *over* the push law without changing node semantics.
- Ops declare `spatial`/`temporal`/`expensive` traits (4.1/4.2); the cache is opt-out per node
  (Resolve parity: user can mark a node "cache").
- Temporal ops take an explicit **time/context** parameter (frame number + fps + deterministic
  seed) so the cache key is honest across scrubbing — the same discipline that makes grain
  reproducible in export.
- GPU (Phase 7): per-op `CPU::apply` + `GPU::shader` twin behind one registry dispatch; cached
  GPU results are texture-backed. The headless invariant is preserved: the *law* is CPU and is
  what tests pin; the shader twin is checked by the existing `gpu_grade`-style parity method.

### 5.3 What stays out (and why)

- Compound/shared nodes (Resolve compound+shared, Nuke groups, Blender node groups, AE precomps):
  v2 — but serialization should already round-trip a node-with-subgraph, so later addition is a
  load-order change, not a format break.
- Scalar/value wires, multichannel/EXR passes, generators, region-of-definition budgeting.
- OFX C ABI hosting (color.md §3.7, deliberate).

---

## Part VI — Phased plan (mapped onto `color-grading-phases.md`)

All phases keep the repo invariants: headless (`check_qtdep`), zero-warning, `ctest` pinned.

**Phase 6 (in flight — finish the canvas over the real model)**
- **Porter–Duff layer law — DONE (2026-09-11).** New headless law module
  `core/include/canvas/core/grade_graph/composite.hpp` + `composite.cpp`
  (`CompositeOp` added beside `BlendMode` in `graph.hpp`; per-layer Node fields
  `composite_op`/`additive`; JSON round-trip in `serialize.cpp`): `blend_into`
  is now true Over-with-blend (alpha recomputed as `as + ab(1-as)`, not copied
  from the backdrop), the Layer Mixer folds `key * opacity * layer_alpha` into
  source coverage and composites premultiplied, the W3C blend equation carries
  the Photoshop family, Disjoint clamps alpha, the Fusion additive↔subtractive
  knob is wired end-to-end, and `composite_test` pins all 8 operators + the
  degenerate opaque case == legacy replace (regression: `graph_test`,
  `graph_edit_test`, `lut_test` all green). Note on the consequent alpha law:
  stacking `kOver` layers of a *semi-transparent* source now builds coverage
  (`ao = as + ab(1-as)`), which is the honest compositing behavior and the
  thing the straight-lerp never did. `kNormal` is now a true Over — done.
- **`OpKind` op registry — DONE (2026-09-11).** `CorrectMode` widened to the
  canonical op-slot type `OpKind` (deprecated `using CorrectMode = OpKind;`
  alias kept, Node field name/JSON key `correct_mode` unchanged so project
  files and the existing Qt tree compile/load untouched — `roundtrip_test.cpp`
  is user WIP and needed no edit). New headless registry
  `core/grade_graph/op.hpp/.cpp` owns the seam Phase 7 widens: `op_apply`
  (pointwise dispatch, byte-identical to the old evaluator switch — eval.cpp
  now delegates, the colorsci laws are untouched), `op_is_identity` (exact-by-
  params OFX IsIdentity fast-path for every kind: default LGG/offset/CDL and
  empty curves are provably identity), and `op_name`/`op_from_name` (the single
  round-trip + UI name table; serialize.cpp delegates; unknown → kIdentity).
  `op_test` (38 checks) pins parity, identity, names, and tolerant load;
  regression: `composite_test`, `graph_test`, `graph_edit_test` all green
  under strict `-Werror`.
- **Remove-node law** (`edit.hpp/../edit.cpp`): `remove_node` (unwire incident edges, detach serial
  links, chain who-connects-whom), so the canvas Delete key edits the *model*, not the scaffold.
  This unblocks the "can't delete nodes" bug for real, model-backed deletion.
- Wire mousePress/Delete/context-menu on `NodeGraphCanvas` to `edit.cpp` laws.

**Phase 7 (GPU + spatial ops)**
- Spatial registry: blur, glow, sharpen/Mid-Detail (the reserved unsharp slot), film grain
  (deterministic time seed) — CPU reference + shader twin, per-op identity tests.
- Per-node memo cache with dirty-propagation invalidation; `IsIdentity` fast-paths.
- Sequence/timeline-level tree with pipeline-stage placement.

**Phase 8 (effects completion)**
- Light rays, tilt/defocus, vignette, chromatic aberration, halation/bloom; Effects dock list
  becomes a real registry-backed catalog over the clip's graph (start of color.md §2.10).
- OFX-host groundwork only if a third-party ecosystem is needed (out of scope per §3.7 otherwise).

**v2 (post-Phase-8)**
- Compound/shared nodes; scalar value wires; multichannel/EXR; OFX hosting; generators.

---

## Part VII — Sources

- DaVinci Resolve 18.6 Reference Manual — "Layer Mixer Node Structures" (steakunderwater mirror of
  documents.blackmagicdesign.com), incl. "lowest input = top priority" language.
- DaVinci Resolve 18.6 Reference Manual — "MultiMerge", incl. layer-list top-to-bottom priority and
  per-layer Merge controls.
- DaVinci Resolve Advanced Panel User Manual — "Add Layer… higher mix priority", mixer morphing,
  "OFX Alpha".
- Fusion 9 User Manual / Fusion 17 User Manual ("Compositing Layers in Fusion"; Merge as the main
  compositing tool; additive/subtractive merging; Over/In/HeldOut/Atop/XOr/Disjoint/Mask/Stencil
  alpha equations).
- Gaffer documentation — `GafferImage.Merge` operator alpha equations.
- OpenFX 1.5.1 — ofxImageEffect API + Image Effect Contexts (Filter/General/Transition/Retimer/
  Generator), clips, EffectMask, GetFramesNeeded, IsIdentity, RoD/region-wanted.
- Blender 5.2 Manual — Compositor System (image vs scalar data, operation domain, half-float),
  Compositor editor.
- The Foundry Nuke — features (multichannel scanline engine, 32-bit float, BlinkScript), Cache
  Directory docs (automatic per-node + DiskCache).
- FilmLight — Baselight datasheet ("layer architecture… clear view on the underlying node tree",
  blend modes, implicit alpha) + Baselight Editions NUKE datasheet (BLG as a multi-input Nuke node).
- Adobe After Effects documentation — composition/layer rendering order: bottom first, per-layer
  masks → effects → transforms → styles; adjustment layers; precomposing/nesting.
- `docs/color.md` (Nova) — Resolve node types/hierarchy, LGG/CDL/curves math, CST/LUT, scopes,
  pipeline-stage plan, Out-of-scope §3.7.
- `docs/color-grading-phases.md` (Nova) — Phase 2 model/evaluator, Phase 4 wheels, Phase 5 curves,
  Phase 6 canvas, Phase 7 GPU + Mid/Detail.
- `core/include/canvas/core/grade_graph/{graph,edit}.hpp`, `core/src/grade_graph/eval.cpp`
  (Nova) — the current model, edit laws, and evaluator this doc extends.