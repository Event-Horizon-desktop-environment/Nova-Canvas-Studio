# Inspector Video Tab — Full Implementation Plan

**Goal:** Wire every option in the Inspector's Video tab to the model with undo, sharing one
headless transform/composite law across playback, export, and the Inspector — the same pattern
the Audio tab uses (`clip_rate.hpp`, `audio_mix.hpp`, `audio_fade.hpp`).

This plan is **research-driven**: each section states how leading NLEs (DaVinci Resolve editing
page, Premiere) actually define and process the control, the mathematical model behind it, and the
implementation phases. Where a number or formula is claimed, it comes from the cited source, not
from guessing.

**Processing order** follows the Resolve Edit-page Video tab — the categories apply top-to-bottom:

```
1. Retime & Input Scaling   (source → timeline frame/size)
2. Stabilization            (analyzer → per-frame correction warp)
3. Lens Distortion Correction (undistort)
4. Transform                (pitch/yaw → anchor → rotate → scale → position → flip)
5. Cropping                 (culls clip pixels, before framing)
6. Dynamic Zoom             (auto-keyframed reframing = push-in / pan)
7. Composite                (blend mode + opacity onto the stack)
```

Everything below the Retime/Scaling step is a per-clip pixel-space effect; `Composite` is the only
one that mixes across tracks.

---

## Phase 0 — Architecture (shared law, headless, undoable)

All numeric laws live in headless modules under `core/` (or `gui/src/Widgets/` where the module
pattern requires it — check `check_qtdep.sh`'s allowlist), following the `audio_mix.hpp` pattern:
model fields in `Clip` → one law function shared by playback renderer, export renderer, and
Inspector. The Inspector commits every gesture as a single undoable `ICommand` via the existing
snapshot `edit_ops` path (`set_clip_visual` analog of `set_clip_audio_processing`).

New model fields must round-trip in `project.hpp` JSON (bump `kProjectVersion` only if the file
format actually changes shape; field-addition with defaults is backward-compatible).

---

## Phase 1 — Transform (Zoom X/Y, Position X/Y, Rotation, Anchor, Flip H/V, Pitch/Yaw)

### How NLEs define it
- **Position X/Y** — pans the clip in the output frame. Resolve: coordinates in **output pixels,
  origin at frame center**, +X right, +Y up; dragging the clip in the viewer writes these. Range
  is unbounded (clip can be pushed fully off-frame).
- **Rotation** — in degrees, right-hand rule, clockwise-positive when +Y is down (screen space),
  rotating **around the anchor point** (Resolve) / the clip's own center (Premiere). Unbounded
  (full circles allowed; apps normalize display but keep large values in the model).
- **Zoom X/Y** — non-uniform scale about the anchor point; Resolve links them with a chain toggle
  so a normal user nudging one nudges both (Premiere: "Maintain Proportions").
- **Anchor Point X/Y** — the pivot for rotation *and* the center the image shrinks/stretches
  toward when scaling. Default: frame center. This is why changing zoom doesn't move the clip.
- **Flip Horizontal/Vertical** — mirrors about the clip's center line.
- **Pitch / Yaw** — a 3D-style rotation that tilts the image plane toward the viewer instead of
  rotating it flat. **Yaw** turns the plane like a swinging gate (horizontal foreshortening →
  trapezoid), **Pitch** tips it like tilting a camera up/down (vertical foreshortening). Both are
  **perspective** effects — they cannot be expressed as a 2D similarity; the frame must be warped
  by a 3D rotation followed by perspective projection.

### The math
Transform matrix composition, applied to source pixels `p` (origin = anchor `a`):

```
q = T(position) · R(rotation) · S(scale_x, scale_y) · T(−anchor) · p
```
then flip = negate the relevant source axis before scaling; order flip *before* scale so aspect is
respected. Pitch/yaw/roll run on the 3D vector (x, y, 1) first (Resolve order: pitch, yaw, roll,
then the 2D anchor/rotate/scale/position on the projected result).

3D rotations (right-handed, source: lavalle.pl/vr/node77.html):

```
Rz(roll) = [cosθ −sinθ 0      Rx(pitch) = [1   0      0
             sinθ  cosθ 0                   0   cosθ −sinθ
             0     0 1]                     0   sinθ  cosθ]

Ry(yaw)  = [cosθ  0  sinθ
             0    1  0
            −sinθ 0  cosθ]
```

A rotated plane point `(x, y, z=0)` maps to `(x', y', z')`; the visible frame is the perspective
projection `(x'/z'p, y'/z'p)` (divide by an effective distance, e.g. `z'p = z' + d`). Net effect:
yaw of θ compresses horizontal scale by ~`cos θ` at center and slants the edges — a projective
transformation, cheap in a fragment shader (one 3×3 matrix + divide).

### Implementation
1. Model: reuse existing `scale_x/scale_y` (link via new `bool transform_link_scale`), `pos_x`,
   `pos_y`, `rotation_deg`, `anchor_dx/dy`, `flip_h/flip_v` (already on `Clip`); add `pitch_deg`,
   `yaw_deg` (defaults 0). Ranges stay in `core/include/canvas/core/timeline/visual.hpp`.
2. New headless law: `transform_matrix(clip, out_w, out_h) → QMatrix4x4`-shaped float[16] (or a
   small struct wrapping row-major floats + perspective-divide flag — keep Qt-free).
3. Playback + export: feed it into the existing viewer shader / renderer warp currently used for
   scale/opacity so rotation/anchors come free; **verify against Resolve anchor semantics** (scale
   keeps the anchor pixel pinned) with a unit test.
4. Inspector: Position/Rotation/Zoom sliders+spins (Resolve ranges: rotation ±360 with free spin,
   position ±frame, zoom 0..10 default 1), chain toggle, Flip buttons. Pitch/Yaw sliders ±90°.
   Defaults all 0 / 1 so identity = current behavior (backward-compatible).

---

## Phase 2 — Cropping

### How NLEs define it
Resolve: **Crop** culls source pixels (Left/Right/Top/Bottom), applied to the *displayed* clip after
transform; Premiere ships it as a Video Effect ("Crop") with % each edge and reusable handles.
Cropping never distorts; it only chooses a sub-rectangle of what Transform produced.

### Math / pipeline
`crop=out_w:out_h:x:y` on the transformed frame (ffmpeg crop semantics: rect + top-left offset,
centered when offsets omitted). Cropping *before* an autoscale is the standard aspect-change
recipe (ffmpeg: "Apply the crop first," then `scale` / `pad`) — see Phase 8 for the interplay with
Retime & Scaling's `Crop` sizing mode.

### Implementation
1. Model: `crop_left/right/top/bottom` in **clip pixels** (or `0..1` fraction of the transformed
   frame; pick pixels to match Position conventions), defaults 0.
2. Law: `crop_rect(clip, frame_w, frame_h)` in the Phase 1 transform law — clamps to `[0, size]`,
   `w' = w − l − r`, `h' = h − t − b`, offset `(l, t)`.
3. Inspector: four sliders/spins per edge + a "reset" button; commit through `edit_ops` snapshot.
4. Viewer: draw a crop wireframe when a clip with non-zero crop is selected.

---

## Phase 3 — Dynamic Zoom

### How NLEs define it
Resolve's Dynamic Zoom (context-menu / Inspector) is a **single-shot Ken Burns effect**: it
auto-creates the animation and shows two adjustable boxes — a **green start box** and a **red end
box** (source: cutsio.com/blog/davinci-resolve-dynamic-zoom-ken-burns-effect). Resolve push-in:
green box is *larger* than red (or equal), so the framing tightens; pull-out is the inverse.
Pan: both boxes the same size, red at a different location. Boxes interpolate across the clip's
duration with eased (smoothstep-like) motion; the user moves start/end frames to retime the motion.
It is **non-destructive** — implemented internally as keyframed Transform (scale/position/rotation).

### Math
In a `rect→transform` conversion: for framing rect `(x, y, w, h)` at time `t`, the box that should
map onto the full output frame defines a **scale** `s(t) = out_w / w(t)` and **position**
`pos(t) = center(rect(t)) − frame_center` (plus the anchor/rotation variants). Interpolation of the
four rect corners (Resolve interpolates the box, which handles push-in-with-pan naturally) with an
ease gives smooth scale+position per frame.

### Implementation
1. Model: `dyn_zoom.enabled`, `start_rect {x,y,w,h}`, `end_rect`, `easing` (default smoothstep),
   `reverse`. Store rects in clip-pixel space at the (un-cropped) source size.
2. Law: `dyn_zoom(zoom, tl_frame, clip_tl_in/out) → {scale, pos, rot}` feeding the Phase 1 law
   (i.e. Dynamic Zoom just writes transform inputs per frame). Apply **after** Crop; it is a
   reframe, not a cull.
3. **No keyframe system needed**: two implicit keyframes (clip in/out) eval'd by the law covers
   Resolve-parity. A general keyframe/tangent engine is a separate later phase (note it, don't
   build it here).

---

## Phase 4 — Composite (Composite Mode + Opacity)

### How NLEs define it
Resolve: Composite Mode dropdown (Normal, Add, Subtract, Multiply, Screen, Overlay, Difference,
HSL, ...) + Opacity %. The mode picks the per-pixel operator; Opacity is the source alpha.

### The math (W3C Compositing-1, w3.org/TR/compositing-1)
Premultiplied source-over compositing of source `Cs,αs` over backdrop `Cb,αb`:

```
co = Cs·αs + Cb·αb·(1 − αs)
αo = αs + αb·(1 − αs)
```

For the non-Normal modes, the blend function operates on **non-premultiplied** color, then the
result is composited over the backdrop using source-over with the source's alpha:
Normal `Cs`, Add `Cs + Cb` (clamped), Multiply `Cs·Cb`,
Screen `Cs + Cb − Cs·Cb`,
Overlay `(Cb ≤ 0.5) ? 2·Cs·Cb : 1 − 2·(1−Cs)·(1−Cb)` (separable, per-channel).

### Implementation
1. `blend_mode` already exists on `Clip` (`Normal/Add/Multiply/Screen/Overlay`), `opacity` exists —
   both already reach the renderer per AGENTS.md. **Verify** the export/playback composite applies
   the mode *and* scales the clip's alpha by `opacity` before the W3C operator, in the correct
   track order (top track last / first-active-wins per the app's convention).
2. GPU path: blend is only needed when ≥2 active layers or `opacity < 1`; keep the single-clip fast
   path but honor `opacity`.
3. Unit tests: port the W3C test vectors (co/αo above) for the 5 modes into `composite` core test.

---

## Phase 5 — Speed Change (video side)

### How NLEs define it
Resolve Speed Change (Inspector) + Retime Controls (clip): a percentage multiplier (default 100%,
range ±), Reverse, and Ripple Timeline (length-changing edit). Frames missing after resampling are
generated by the **Retime Process** chosen under Retime & Scaling (Phase 8):
- **Nearest** — repeat nearest source frame (slow) / drop frames (fast). Cheapest.
- **Frame Blend** — dissolve adjacent source frames by the fractional offset (`out = I(n)·(1−f) +
  I(n+1)·f`); smooths mild slow-mo, ghosts on fast motion. (source: dvblend.com / BM forum
  posts above).
- **Optical Flow** — estimate motion per pixel, warp the source frames into the new frames; the
  highest quality, heavy cost, artifacts on complex (hair/water) motion (sources: Resolve manual,
  elements.tv).

### Implementation
1. Model: video `retime_process` enum `{Nearest, FrameBlend, OpticalFlow}` (reuse existing
   `speed_factor`/`speed_enabled` law from `clip_rate.hpp` for `spd`).
2. A retime law `video_sample_frames(spd, tl_frame, src_in) → {frame_idx, fraction}` in core
   (mirror of `clip_rate`'s fractional source-time math).
3. Renderer/playback: at `spd != 1`,
   - Nearest → decode floor index.
   - FrameBlend → decode index n and n+1, mix with `fraction` (needs the second frame; the
     existing decoder/`FrameCache` already caches adjacent frames).
   - Optical Flow → **Phase 1 of this feature = FFmpeg's `minterpolate` filter** (motion-compensated
     interpolation already in the required FFmpeg dep) for the affected range at export; playback
     falls back to FrameBlend until a realtime flow path exists. Phase 2 = custom dense-flow warp.
4. Inspector: Speed factor slider+spin (already on Audio tab), Reverse toggle, Retime Process combo,
   Ripple Timeline through the existing timeline-length edit op.

---

## Phase 6 — Stabilization

### How NLEs define it
Resolve Image Stabilization: Analyzer → internal per-frame 2D transforms, then an apply pass with
**Smooth / Amount**, **Method** (Perspective/Affine/Similarity/Translation), **Zoom** and border
**(Crop Ratio)**. Apply + Analyze are split; the user can dial strength up to fully locked,
re-analyze, and crop the uncovered borders.

### The math (MIT video-stabilization course notes, people.csail.mit.edu/fredo/comp-photo-book)
1. Track Shi-Tomasi corners with KLT forward/backward; drop short tracks.
2. Per consecutive frame pair, fit a 2D transform `F_t` (translation → similarity → affine →
   homography, fidelity ladder; RANSAC over tracks for robustness).
3. Cumulative camera path `C_t = F_t·F_{t−1}·…·F_1`.
4. Smooth the path (LPF/polynomial/Savitzky–Golay — Resolve default is smoothed by amount) → `P_t`.
5. Correction warp per frame: `W_t = P_t · C_t^−1`.
6. Draw each frame with `W_t`; border seams are covered by scaling up (Zoom) and cropping the now-
   stable inner rectangle (Crop Ratio).

### Implementation
1. Model: `stabilize.enabled`, `method` (`Translation|Similarity|Affine|Perspective`),
   `smooth_amount` (0..1), `zoom`, `crop_ratio`.
2. Analyzer (headless, on a background worker like ThumbnailService): compute `C_t`/`P_t`/`W_t` over
   the clip's source range using `cv`-free code (Eigen-free: hand-rolled 2D least-squares + 3×3 mat
   multiplications; or a small direct-dep opt-in). Store one 3×3 (or 2×3) warp per source frame on
   the clip (`std::vector<float>`); recompute on media change; cancel on edit.
3. Apply: playback/export warp each timeline frame by `W[src_frame(t)]` in the Phase 1 shader (GPU)
   or CPU warp; then apply Zoom scale + crop.
4. Inspector: Analyze button (spins), Method combo, Smooth slider, Zoom, Crop Ratio; state through
   the one undoable edit op.

---

## Phase 7 — Lens Distortion Correction

### How NLEs define it
Resolve adds Lens Distortion Correction to the Edit inspector (and Color page Vignette/openFX);
it undoes barrel/pincushion and fisheye optics so corrected lines stay straight. It rides before
Transform in the processing order.

### The math (Brown–Conrady radial model; source: pmc.ncbi.nlm.nih.gov/articles/PMC4934233/)
Normalized coords about the optical center `(cx, cy)`, `r² = x² + y²`:

```
x_dist = x·(1 + k1·r² + k2·r⁴ + k3·r⁶)      (barrel: k1 < 0, pincushion: k1 > 0)
y_dist = y·(1 + k1·r² + k2·r⁴ + k3·r⁶)
```

The *undistort* mapping for an output pixel solves the inverse (iterate the forward model a few
times, or Newton) to find the source pixel; Resolve also offers a "fisheye → rectilinear" option.

### Implementation
1. Model: `lens.enabled`, `amount` (barrel − / pincushion +), `k2/k3` (extra terms, defaults 0),
   `cx/cy` (optical center, default frame center). One `amount` slider → `k1` for v0.
2. Law: `lens_distort(p_in, k, center) → p_out` in core; a pure coordinate maps, so it's a 1-line
   fragment-shader change (recompute the quad's texture coordinate) for playback/GPU, and a CPU
   inverse-map for the CPU export path.
3. Inspector: enable toggle + Amount slider (+ advanced k2/k3, optical-center spinny). Unit-test
   barrel vs pincushion sign on a synthetic grid.

---

## Phase 8 — Retime and Scaling (Input Sizing)

### How NLEs define it
Resolve "Retime and Scaling": **Retime Process & Motion Estimation** (see Phase 5) + **Scaling**
and **Resize Filter** for clips whose resolution/aspect ≠ project (source: Resolve 18 manual,
part1258.htm). Scaling options: **Crop, Fit, Fill, Stretch** — the classic
contain/cover/stretch family:
- **Crop** — scale up to *fill* the output, center-crop the overflow (Resolve default; no bars, no
  distortion, loses edges).
- **Fit** — scale to *contain*; letterbox/pillarbox deadspace is left transparent/black.
- **Fill** — scale to fill on both axes; keep all pixels → distortion (stretch) when ratios differ.
- **Stretch** — force exact output dimensions, ignoring aspect (same as Fill; exposed as its own
  preset in Resolve).
Resize **Filter**: Sharper (≈ Lanczos-family, best for upscale), Smooth(er) (pre-blur/binomial),
plus plain Bilinear/Bicubic — the kernel that sizes pixels during any of the above.

Standard ffmpeg idiom for Crop-mode: `scale=…:force_original_aspect_ratio=increase, crop=W:H`
("scale to fill, then center-crop"); for Fit:
`scale=…:decrease, pad=W:H:(ow−iw)/2:(oh−ih)/2`.

### Implementation
1. Model: `scaling_mode {Crop, Fit, Fill, Stretch}` (default Crop = current app behavior),
   `resize_filter {Bilinear, Bicubic, Sharper, Smoother}`.
2. Headless `sizing.hpp`: `sizing(source_w,h, out_w,out_h, mode) → {scale, offset, pad}` — the
   contain/cover clamp math; feeds the head of the Phase 1 chain for every layer, and the media
   pool / deliver previews reuse it.
3. Filter selection hits FFmpeg's `sws_ctx` (flags: `SWS_BILINEAR/SWS_BICUBIC/SWS_LANCZOS/…`) in
   `video_decoder`/`renderer` when it resizes; expose through the RR law so playback/export agree.
4. Retime Process (Phase 5) default: project setting (always Nearest today); per-clip override
   combo.

---

## Cross-cutting notes
- Every phase adds fields to `Clip` → must round-trip in `project.hpp` (defaults keep old files
  valid) and be **clamped/legal in `edit_ops`** per `visual.hpp` ranges so Inspector, playback, and
  export never drift.
- All new law modules are Qt-free (append to `check_qtdep.sh` allowlist, wire a `canvas_add_headless_test`).
- Undo = one snapshot cmd per committed gesture; renable playback-worker immutable-copy handoff
  (already the edit path).
- **"Wired" today** (verify before assuming): Composite Mode + Opacity reach the renderer; Zoom,
  Position, Rotation, Anchor, Flip reach the renderer but the Inspector pages may still be sparse.
  Everything else is placeholder UI.
- Reserved follow-up (out of scope here): a general keyframe/tangent engine (Dynamic Zoom uses only
  its two implicit keyframes for now), realtime optical-flow intermediate frames, rolling-shutter
  correction in the stabilization analyzer, and a neural Speed Warp equivalent.