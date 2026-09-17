# Nova Canvas Studio — Roadmap

**Dated:** 2026-09-16 · **Source of truth links:** [docs/FEATURES.md](docs/FEATURES.md) (what exists today), [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

This roadmap compares Nova Canvas Studio against the three NLEs it most often
gets measured against — **Kdenlive** (25.04/26.04, MLT-based), **Shotcut**
(25.xx feature page), and **DaVinci Resolve 20/21 Studio** — and translates
the gaps into a phased plan sized against our architecture. It is not a
feature wishlist; every item names the existing Nova machinery it builds on.

## Where we sit

Nova is an alpha, Linux-only, C++20/Qt6/FFmpeg NLE with an unusually strong
floor for its age:

- **Solid editing core** — undoable snapshot-based edit ops, linked A/V,
  per-clip speed 0.1–10× + pitch (WSOLA), transitions with a handle editor,
  snapping, markers, per-clip volume lines.
- **A real color page** — node-graph grading, wheels, curves, five live
  scopes, 3D-LUT baking, a fused CUDA grade/resize/title kernel. This is the
  area where we already think like Resolve.
- **Local AI already shipping** — whisper.cpp transcription → captions +
  transcripts on the timeline, and voice isolation.
- **HW everywhere it counts** — cuda→vaapi→qsv→vulkan decode probe, NVENC /
  VAAPI / QSV / CPU encode, GPU optional everywhere.
- **64-test suite, 62 passing**, warning-free Debug + Release builds,
  headless core split from the GUI.

The honest gaps vs. the field: **no general effect/keyframe system**, **no
trim toolset beyond ripple/lift**, **no multi-cam**, **no proxies/render
cache**, **no interchange (EDL/AAF/XML)**, **no audio mixing beyond per-clip
volume/pan**, and **no HDR/10-bit pipeline**.

## Capability gap matrix

| Capability | Nova | Kdenlive | Shotcut | Resolve Studio |
|---|---|---|---|---|
| Undoable editing, linked A/V, snaps | ✅ | ✅ | ✅ | ✅ |
| Snap-based edit ops (lift/ripple/blade/move) | ✅ | ✅ | ✅ | ✅ |
| Ripple / rolling / slip / slide trims | ⚠️ ripple+lift only | ✅ | ✅ | ✅ |
| 3-point editing / overwrite modes | 🚫 | ✅ | ✅ | ✅ |
| Multi-track unlimited, hide/mute/collapse | ⚠️ fixed V/A lanes | ✅ | ✅ | ✅ |
| Multi-cam clips + angle switching (AI SmartSwitch) | 🚫 | ✅ | 🚫 | ✅ |
| Audio-sync multi-clip (group/align) | ⚠️ A/V pairs only | ⚠️ external | ✅ | ✅ |
| Clip grouping, nudge/move groups, split/rejoin | 🚫 | ⚠️ | ✅ | ✅ |
| Markers/ranges → export-from-range, chapters | ⚠️ bookmarks only | ✅ | ✅ | ✅ |
| Multiple timelines / sequences | 🚫 single Sequence | ✅ | ✅ | ✅ |
| General per-clip effect stack | 🚫 | ✅ hundreds | ✅ hundreds | ✅ ResolveFX |
| Keyframes + easing on effects/transforms | 🚫 | ✅ | ✅ | ✅ |
| Effect presets / save-custom-effect | 🚫 | ✅ | ✅ | ✅ |
| Masks / regions on effects | 🚫 | ✅ | ✅ | ✅ |
| Compositing/blend modes | ✅ 5 blend modes | ✅ | ✅ ~20 | ✅ |
| Node-graph color grading | ✅ | 🚫 | 🚫 | ✅ |
| Color wheels / curves / scopes | ✅ | ⚠️ 4 scopes | ✅ 5 scopes | ✅ pro |
| Qualifiers / secondaries / power windows | 🚫 | 🚫 | 🚫 | ✅ |
| HDR (PQ/HLG, tone mapping, 10-bit linear) | 🚫 | ⚠️ | ✅ | ✅ |
| Color management (ACES/RCM-ish) | ⚠️ embedded | 🚫 | ⚠️ | ✅ |
| Magic Mask / object tracking / Super Scale | 🚫 | 🚫 | ⚠️ motion tracker | ✅ AI |
| AI transcription → captions/subtitles | ✅ whisper | ✅ | ✅ | ✅ |
| **Text-based editing from transcripts** | 🚫 | 🚫 | 🚫 | ✅ |
| Scene cut / silence detection, auto-delete | 🚫 | 🚫 | 🚫 | ✅ |
| Voice isolation / dialogue tools | ✅ voice isolation | 🚫 | 🚫 | ✅ |
| Audio mixing: busses/sends/dynamics/automation | 🚫 per-clip only | ⚠️ | ⚠️ | ✅ Fairlight |
| Audio scopes (meter/loudness/spectrum) | 🚫 | ✅ | ✅ | ✅ |
| Surround / Ambisonics | 🚫 | ⚠️ | ✅ | ✅ |
| Proxies / optimized media workflow | 🚫 | ✅ | ✅ | ✅ |
| Render/preview cache management | 🚫 | ✅ | ✅ | ✅ |
| Title editor (2D, scroll, typewriter) | ⚠️ sprites only | ✅ | ✅ | ✅ Text+ |
| Motion-graphics animation (Lottie/Glaxnimate) | 🚫 | ✅ | ✅ | ✅ |
| Subtitle import SRT/VTT/ASS + burn-in | ⚠️ local gen only | ✅ | ✅ | ✅ |
| Image sequences / stills / multi-format timeline | 🚫 | ✅ | ✅ | ✅ |
| Webcam / audio / voiceover capture | 🚫 | ✅ | ✅ | ✅ |
| EDL / AAF / XML interchange | 🚫 | ⚠️ FCPXML | ✅ EDL out | ✅ pro |
| Batch jobs / image-sequence / still export | 🚫 | ✅ | ✅ | ✅ |
| Loudness normalization / broadcast delivery | 🚫 | ⚠️ | ⚠️ | ✅ |
| Editable shortcuts, action search, layouts | 🚫 | ✅ | ✅ | ✅ |
| Cloud collaboration / multi-user | 🚫 | 🚫 | 🚫 | ✅ (out of scope for us) |

Legend: ✅ present · ⚠️ partial · 🚫 absent.

## Missing features by domain

Effort is S/M/L/XL against our current codebase. "Builds on" lists the Nova
machinery that already exists so each item is an extension, not a new world.

### 1. Editing & timeline — foundation stones

| # | Feature | Effort | Builds on |
|---|---|---|---|
| E1 | **Rolling / slip / slide trims** (ripple exists) | M | `edit_ops` trim + `timeline_drag::DragController` session model; undo via `ICommand` |
| E2 | **3-point editing + overwrite/insert modes** | S | existing place/overwrite ops + source preview in/out |
| E3 | **Clip grouping + group nudge/move, split/rejoin** | M | `linked_id` grouping already lifts A/V pairs — generalize to arbitrary groups |
| E4 | **Timeline markers → named ranges, export-from-range, chapter output** | S | `Sequence::bookmarks` already exists — extend to ranges + Deliver mapping |
| E5 | **Multiple sequences/timelines with bin-based nesting** | L | `Project` holds one `Sequence` — refactor to a sequence list; `MediaId` indirection makes it tractable |
| E6 | **Track hide/mute/rename/lock + flexible insert/reorder** | S | `Track{locked, solo, name}` present — add hidden/mute fields + drag-reorder |
| E7 | **Frame-accurate source preview in/out + tape-style shuttling** | M | `source_preview_model_test` seam exists |
| E8 | **Time remap / keyframed speed ramps, reverse, freeze frame** | M | per-clip `speed_factor` law + WSOLA `TimeStretchBank` — add keyframe tracks, interpolate `spd` per-frame |
| E9 | **Preview/render cache (RT-cache) with management** | M | `FrameCache` LRU + scrub-preview LRU exist; add disk render-cache tiles keyed by (clip, grade, effects) |
| E10 | **Proxy / optimized media workflow** | M | low-res `kPreviewMaxDim` cap + `ThumbnailService` disk cache scaffold the pattern; `sw_encode` already makes proxies fast |

### 2. Multicam — E11 (single workstream)

Kdenlive and Resolve both ship it; Shotcut does not. High value for the
"streamer/collaborator" user we can win.

- **Multi-angle source + sync** (by audio or by timecode), **multicam viewer
  with angle switching** (⚠️ Duplicate Resolve-style), **flatten to timeline**.
- Builds on: `av_reanchor` audio sync test seam, existing per-media decoders,
  `clip_rate` retiming. Effort **L** — split as E11a (sync → multicam bin),
  E11b (viewer/switcher), E11c (flatten).

### 3. Effects system — the biggest gap

Both FOSS apps ship hundreds of effects; Resolve ships ResolveFX. Nova has
per-clip transform/composite/opacity/blend and a node **grade graph for one
clip at a time**, but **no ordered, keyframable effect stack on timeline
clips**. This must be a first-class platform feature, built headlessly.

| # | Feature | Effort | Builds on |
|---|---|---|---|
| F1 | **Effect stack data model** (ordered chain, enable/disable, params) | L | `Clip` + new `effect_ops` edit module; renderer `render_video_frame` already walks per-clip transform — generalize into a chain |
| F2 | **Keyframe tracks (linear/smooth/easing) + timeline keyframe UI** | L | volume-line inline drawing pattern + `transition_handle_editor` drag-session pattern |
| F3 | **Effect v1 library via avfilter/frei0r** — crop, blur, sharpen, color, keystone, vignette, grain; audio gain/EQ/compressor | M each | FFmpeg already linked; swresample/swscale in engine |
| F4 | **Effect + keyframe presets, save-custom-effect, favorites** | M | JSON project serialization + Deliver presets pattern |
| F5 | **Masks/regions (apply effect to a sub-region), roto v1** | L | grade `composite` module + node graph canvas |
| F6 | **Blend modes → full set** (currently 5) | S | `visual.hpp` `kBlendModeCount=5` — extend law + renderer + Inspector |

> **Sequencing rule:** F1 + F2 first (the platform), then F3/F4 (the library),
> F5/F6 later. Without F1/F2 we will keep bolting single-purpose knobs onto
> the model — that is the ceiling we must break.

### 4. Color — already strong, extend deliberately

Our color page is the Resolve-like differentiator. Priority is depth, not
breadth.

| # | Feature | Effort | Builds on |
|---|---|---|---|
| C1 | **Qualifiers (HSL/HSV secondaries) + power windows** | L | wheels/curves infra + `grade_graph` nodes; add qualifier matte + window nodes |
| C2 | **Object/point tracker → drive windows, mask, transform** | L | CUDA/VAAPI kernels + node graph evaluator |
| C3 | **LUT gallery + .cube/.png import/export, per-clip LUT node** | M | `lut` bake engine already byte-exact |
| C4 | **Scene-cut detection + auto-add-edit** | M | FFmpeg decode already threaded; histogram diff law in `histogram.hpp` |
| C5 | **Color management: working space + ACES-style input→output transforms** | L | `gpu/colorspace.hpp` is the single color-law seam — extend it |
| C6 | **AI: Magic-Mask-style subject selection (v1 = interactive graph cut / SAM-clip)** | XL | long pole; park after E-series and F-platform |
| C7 | **Stills gallery + shot-match (auto grade reference)** | M | project JSON + `lut`/grade graph serialization |

### 5. Audio — from "per-clip volume" to a mixer

| # | Feature | Effort | Builds on |
|---|---|---|---|
| A1 | **Audio busses + sends + track mixing** | L | `AudioPipeline` mix path + `audio_targets` resolution |
| A2 | **Dynamics: compressor/limiter/gate/de-esser** | M | new DSP modules beside `equalizer`; laws + headless tests like `equalizer` |
| A3 | **Automation lanes (volume/pan keyframed)** | L | volume-line already draws+edits one param — generalize to pan + busses |
| A4 | **Audio scopes: peak meter, loudness (EBU R128), spectrum** | M | `histogram_scope`/`scope_common` widgets are the pattern; `swresample` metering |
| A5 | **Loudness normalization on export** | S | `audio_mix` gain law + Deliver settings → `ExportSettings` |
| A6 | **Noise reduction (RNNoise-style) + spectral denoise** | M | `voice_isolation` DSP pattern |
| A7 | **AI: IntelliCut-lite (remove silence, rip per-speaker)** | M | whisper transcripts already segment speakers/timestamps |

### 6. AI — sharpen the existing wedge

We already ship local whisper + voice isolation (neither FOSS competitor
mixes both on-timeline). Extend in this order:

| # | Feature | Effort | Builds on |
|---|---|---|---|
| N1 | **Text-based editing: jump/trim/delete from transcript, subtitle-anchored cuts** | M | `transcript`/`captions` model already on the timeline strip |
| N2 | **Smart subtitles: animated word highlight, styling presets** | M | title sprite rasterizer + captions model |
| N3 | **Scene/silence detection → smart ripple-delete + beat-edit** | M | C4 + whisper timestamps |
| N4 | **AI upscale (Super Scale-style 2×/4×)** | L | CUDA `nv12Resize`/grade fusion surf + `ga` docs; CPU fallback via quality resample |
| N5 | **AI denoise / reframe-crop** | L | same GPU seam |
| N6 | **Dialogue matcher / music-beat tools** | XL | park — after N1–N5 |

### 7. Deliver & interchange

| # | Feature | Effort | Builds on |
|---|---|---|---|
| D1 | **EDL (CMX3600) export + import** | S | frame-accurate timeline model + timecode lib |
| D2 | **Subtitle burn-in on export** | S | title blend kernel already runs on GPU; CPU parity exists |
| D3 | **Markers/ranges → chapters + embed into MP4/WebM** | S | E4 + export mux |
| D4 | **Still-frame + image-sequence export** | S | `render_video_frame` single-shot exists |
| D5 | **Batch/render-all queue priority + pause** | S | `RenderQueue` worker + `render_queue_panel` |
| D6 | **AAF/XML interchange (round-trip with Premiere/Resolve)** | L | EDL path first; document format, then bidirectional |
| D7 | **HDR10/HLG encode pass-through + tone mapping** | L | requires C5 + 10-bit render path — long pole |
| D8 | **Autosave + crash recovery + project archive/backup** | S | project serialization + close-prompt already present |
| D9 | **PSNR/SSIM QC tool** | S | FFmpeg already linked; decode both files + metric |

### 8. Platform/UX polish

| # | Feature | Effort | Builds on |
|---|---|---|---|
| U1 | **Editable keyboard shortcuts (persisted)** | M | AppActions key handler → QSettings profile |
| U2 | **Action search box (Ctrl+P-style)** | S | menu wiring already centralized in `ShellMenus` |
| U3 | **Saveable workspace layouts** | M | docks are programmatic (`MainWindowShell`) — save geometry state |
| U4 | **Multi-format timeline (mixed fps/res into one Sequence)** | M | `media_rate_at` + per-clip rate already exist; fps is sequence-global today |
| U5 | **Image sequences + still import/export** | L | media pipeline + D4 |
| U6 | **Webcam/audio/voiceover capture** | L | depends on ALSA/PipeWire capture; low priority |
| U7 | **i18n** | XL | park — after U1–U3; Qt tr() is already used throughout |

## Phased plan — easiest → hardest

The phases are a **difficulty ramp**: Phase 1 is nothing but small,
independent wins (each roughly a weekend, zero architectural risk), and every
phase after is harder than the last. Two rules bend the pure sort:

- **Dependencies win.** A medium item that needs a large platform (the effect
  library) waits for that platform, even though it is easier on its own.
- **Housekeeping rides the ramp, it does not preempt it.** `-Werror` is
  Phase-1 class, the audio-decode-path dedup is Phase-2 class, the unfinished
  Vulkan render kernel is Phase-6 class — it gets parity work at its own
  difficulty tier, not priority-wart work ahead of shipping features.

Each phase is complete only when its headless modules carry tests and the
tree is warning-free in Debug + Release (existing discipline).

### Phase 1 — Low-hanging fruit (S) — *"a noticeably more complete NLE, no new architecture"*
- E2 3-point editing + overwrite/insert modes
- E4 markers → named ranges, export-from-range
- E6 track hide/mute/rename + flexible insert/reorder
- F6 full blend-mode set (5 → ~20)
- D1 EDL (CMX3600) export
- D2 subtitle burn-in on export
- D3 ranges → chapters embedded in MP4/WebM
- D4 still-frame + image-sequence export
- D5 render-queue priority + pause
- D8 autosave + crash recovery + project archive/backup
- D9 PSNR/SSIM QC tool
- A5 loudness normalization on export
- U2 action search box
- **Rider:** `-Werror` on all targets

### Phase 2 — Editing & interaction depth (M)
- E1 rolling / slip / slide trims (ripple exists — add the trio)
- E3 clip grouping + group nudge / split / rejoin
- E7 source-preview in/out + tape-style shuttling
- E8 time remap — keyframed speed ramps, reverse, freeze frame
- E9 preview / render cache with management
- E10 proxy / optimized media workflow
- U1 editable keyboard shortcuts (persisted)
- U3 saveable workspace layouts
- U4 multi-format timeline (mixed fps/res into one Sequence)
- A4 audio scopes (peak meter, EBU R128 loudness, spectrum)
- **Rider:** dedupe the two audio-decode paths

### Phase 3 — The AI wedge (M) — *first differentiators, everything already exists*
- N1 text-based editing from transcripts (jump/trim/delete in the timeline strip)
- N2 smart subtitles — animated word highlight, styling presets
- N3 silence / beat detection → smart ripple-delete + beat edit
- A7 dialogue rip / ADR-lite (IntelliCut checkerboard per speaker)

### Phase 4 — Color & audio deep cuts (M)
- C3 LUT gallery + per-clip LUT node (.cube/.png import/export)
- C4 scene-cut detection → auto-add-edit
- C7 stills gallery + shot-match auto-grade
- A2 dynamics — compressor / limiter / gate / de-esser
- A6 noise reduction (RNNoise-style) + spectral denoise

### Phase 5 — The effects platform (L) — *the commitment phase*
- F1 effect-stack data model (ordered chain, enable/disable, params) — headless
- F2 keyframe tracks (linear / smooth / easing) + timeline keyframe UI
- F3 effect library v1 via avfilter/frei0r — crop, blur, sharpen, color,
  keystone, vignette, grain; audio gain / EQ / dynamics
- F4 effect + keyframe presets, save-custom-effect, favorites
- F5 masks / regions v1 (restrict an effect to a sub-region)

After this we are a *real* NLE with an effects platform; everything from
Phase 6 on builds on it.

### Phase 6 — Media infrastructure (L)
- E11 multicam — audio/timecode sync → multicam bin → angle viewer → flatten
- E5 multiple sequences / timelines (project → sequence list)
- U5 image sequences + still import
- A1 audio busses + sends + track mixing
- A3 automation lanes (keyframed volume/pan → busses)
- **Rider:** Vulkan render kernel + retire the two SKIP-stub tests

### Phase 7 — Pro color & delivery (L)
- C1 qualifiers / secondaries + power windows
- C2 object / point tracker (drives windows, masks, transforms)
- C5 color management — working space + ACES-style input→output transforms
- D7 HDR10 / HLG pass-through + tone mapping (needs C5 + the Phase-6 GPU kernel)
- D6 AAF / XML interchange (round-trip with Premiere/Resolve)
- U6 webcam / audio / voiceover capture

### Phase 8 — The AI frontier (XL)
- N4 AI upscale (Super Scale-style 2×/4×)
- N5 AI denoise / smart-reframe
- C6 Magic-Mask-style subject selection + tracking
- N6 dialogue matcher / music-beat tools
- U7 i18n

## Deliberately out of scope

- **Windows/macOS** — Linux only, by design (the project's founding stance).
- **Cloud collaboration / multi-user / shared timelines** — local-first is the
  product identity; local autosave/backups (D8) is the closest we'll go.
- **Hardware control panels / Speed Editor / color panels** — QWERTY-only.
- **SDI/Decklink broadcast monitoring** — niche on Linux desktop; revisit only
  if real demand appears.
- **Full Fusion-grade 3D compositor / USD** — we stop at node-graph 2D
  compositing extended from `grade_graph`; no 3D scene graph.
- **360°/VR tooling** — Shotcut's long-tail area, not our audience.

## How we know it's done

Same bar as everything else in this repo: each feature lands headlessly
(no Qt in its module), with regression tests in the suite, zero warnings in
Debug + Release, and a `docs/FEATURES.md` line flip from "planned" to
"working." The matrix above doubles as the tracking list — the plan is to
open the gap table at the top of each quarter's planning and strike items.