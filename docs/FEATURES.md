# Nova Canvas Studio — Features

Current as of 2026-09-16. Status markers:

- **✅ Working** — implemented and exercised by the test suite and/or regular use.
- **⚠️ Partial / WIP** — usable but with known limits, or on the roadmap and not done.

Verification baseline: the project builds warning-free in Debug and Release
(`./build.sh`), and 62 of 64 CTest tests pass (the two non-passing tests are
Vulkan SKIP stubs owned by the unfinished Vulkan render kernel — see
*Hardware acceleration* below). Tests run with `ctest --test-dir build`.

## Editing (timeline)

- ✅ Undoable edit operations — place, linked A/V place, unlink/link, lift,
  ripple delete, blade, move, transitions, delete-through-edit — on a
  snapshot-based command stack (Ctrl+Z / Ctrl+Shift+Z).
- ✅ Linked video/audio pairs: move, delete, and transition together.
- ✅ Per-clip speed change (0.1×–10×) with pitch-preserving WSOLA time-stretch,
  and pitch shift (±12 semitones / ±100 cents).
- ✅ Audio fades and video transitions handled right on the clip, with a
  drag-session handle editor (preset snap included).
- ✅ Magnetic snapping (clip edges + bookmarks) with zoom-dependent grid
  fallback; toggleable via the transport Snap button.
- ✅ Selection model with linked-mate coalescing and range membership.
- ✅ Clip colours: Resolve-style 12-swatch wheel (Inspector + Clip menu).
- ✅ Per-clip volume line (drag directly on the audio clip) with accurate
  waveform-scaled preview; multi-select audio targets for mass gain edits.
- ✅ Markers/bookmarks (M) and Mark In/Out points (I / O / Alt+X).
- ✅ Clip enable/disable toggle (Ctrl+D), link indicator, blade (B) and
  select (A) tools.
- ✅ Track collapse/expand (per-track chevrons or Collapse/Expand All) as one
  undoable command.
- ✅ Ripple delete (Del) and lift (Shift+Del / Backspace), remove-all-transitions.
- ⚠️ Split/slide/rolling-trim tools are not bound yet; trim is ripple/lift only.

## Timeline UI

- ✅ Pinned top strip: live timecode bar, minimap, ruler with tick labels —
  mouse-transparent so scrubbing works over them.
- ✅ Playhead drawn over the ruler/grid; ruler-click seek and playhead drag.
- ✅ Divider band between video/audio sections: grab to pan the channel stack
  (pull up = clamped at the parked limit, pull down = slide into the void).
- ✅ Filmstrip thumbnails and waveform previews generated asynchronously and
  cached (in-memory + disk).
- ✅ Drag-and-drop: media pool → timeline with a drag ghost and a full
  track-stack drop preview (template lane map on empty timelines, target-lane
  tint, clip ghost + linked A1 audio-mate ghost, drop-frame guide, timecode
  and lane tag; the preview clamps to the content edge at the track header).
- ✅ Magnetically snapped clip drags, trims, and playhead scrubbing.
- ✅ Zoom in/out (Ctrl+scroll), zoom-to-fit (Shift+Z).

## Media pool & project manager

- ✅ Media import (Ctrl+I) with thumbnails and audio waveforms.
- ✅ Drag clips from the pool onto the timeline; external file drops too.
- ✅ Project Manager hub: recent-project cards with card thumbnails, new/open.
- ✅ Project files: `.ncs` (versioned JSON, `canvas_project` key); legacy
  `.ehproj` / `event_horizon_project` files still open.
- ✅ Recent files list (File > Open Recent, 10 entries).

## Playback

- ✅ Multi-threaded playback stack: video decode/composite, audio
  decode/feed/mix, A/V-sync bookkeeping with a drop-to-realtime cap policy.
- ✅ Hardware decode when available: cuda → vaapi → qsv → vulkan probe with
  software fallback; shared device across decoder slots.
- ✅ Frame cache (byte-budgeted LRU) and a low-res scrub-preview cache for
  fast scrubbing.
- ✅ ALSA output with PipeWire fallback; audible-scrub toggle in Preferences.
- ✅ Transport bindings: Space/K/L, J/Left/Right frame steps,
  Shift+Left/Right full-second steps, Home/Go-to-start, End/Go-to-end.
- ✅ Source preview panel with its own decode model.

## Audio

- ✅ Per-clip volume/pan laws shared across playback, export, and the
  Inspector (dB → linear gain, stereo balance, fades) — one source of truth.
- ✅ 6-band parametric EQ with a hand-painted frequency-response graph that
  doubles as an editor (Bands faders or draggable Curves mode).
- ✅ AI voice isolation (local; no uploads).
- ✅ Local AI subtitle generation from any selected audio clip
  (whisper.cpp): generates captions + a transcript on the timeline strip.

## Titles & subtitles

- ✅ Title clips (Add Title, Ctrl+Alt+T) rasterized to a sprite and blended
  onto video during playback and export.
- ✅ Captions model and subtitle dialogs; captions/transcripts persist in the
  project file.

## Colour grading

- ✅ Dedicated Color page with a node-graph editor (add/edit/evaluate nodes,
  undoable), color wheels, curves, and a mini timeline strip.
- ✅ Live scopes: waveform, parade, vectorscope, histogram, chromaticity.
- ✅ Grade LUT baking (Resolve-style 3D LUT): identity byte-exact round-trip,
  gain byte-exact export parity, grid-vs-evaluator parity.
- ✅ BT.601 (SD) colour transform math shared between the software compositor
  and the GPU kernels so the viewer and the export never drift.
- ✅ GPU-accelerated single-clip grading path (CUDA) when a toolkit is present
  (fused resize + grade + title-blend kernels).

## Export / Deliver

- ✅ Deliver page with NLE-style settings: preset names, encoder backends,
  per-format codec/container lists, bitrate visibility.
- ✅ Hardware encoders: NVENC (CUDA), VAAPI (AMD/Intel/NVIDIA), QSV (Intel)
  with CPU software fallback.
- ✅ Background render queue with progress and cancel; renders report elapsed
  wall time.
- ✅ Export audio path honours per-clip speed/pitch (WSOLA), volume/pan, and
  fades; video honours transform/composite/title blends.
- ✅ Export sweep test covers every valid codec×container combination.

## Hardware acceleration

- ✅ Hardware decode probe (cuda → vaapi → qsv → vulkan) with graceful
  software fallback; per-vendor VAAPI registry, surface-ownership contract,
  and rc/preset mapping (headless-tested without a device).
- ✅ CUDA encode + fused NV12 grade/resize/title kernels, VRAM-leak regression
  gate.
- ⚠️ Vulkan compute pipeline is mid-flight: queue-matrix, profile, format,
  interop, and VRAM tests pass, but `scrub_bench_vulkan_test` and
  `visual_render_parity_test` are SKIP stubs until the render kernel lands.
- ✅ GPU is optional everywhere: without CUDA you get full software encode;
  without ALSA/PipeWire you get video but no audio.

## UI / theming

- ✅ Resolve-style page bar: Media / Cut / Edit / Fusion / Color / Fairlight /
  Deliver (Edit, Color, Deliver, and the Project Manager are functional;
  Fusion/Fairlight are placeholder pages).
- ✅ Editor-grade dark theme (HyprDark/Dark/Light) with custom accent and
  playhead colour overrides persisted in Settings (Ctrl+,).
- ✅ Inspector docks: Video (transform/composite), Audio (volume/pan, pitch,
  speed, EQ, voice isolation), Transition, File, Subtitles.
- ✅ Full menu bar (app menu + File/Edit/Trim/Timeline/Clip/Mark/View/
  Playback; Fusion/Color/Fairlight/Workspace shells present).
- ✅ Glassy custom style (QProxyStyle), SVG icon set, rounded popup menus.
- ✅ Keyboard shortcut set documented in `docs/KEYBINDS.md`.

## Known limitations

- Two audio decode paths overlap (`VideoDecoder::decode_audio` vs the
  standalone `AudioDecoder`); playback uses the standalone one — de-duplication
  is planned.
- `AudioOutput::flush()` must join its writer thread before being dropped; this
  is documented in code and worked around at call sites.
- The `I` key doubles as Mark In and Inspector toggle (documented collision).
- Transition-handle preset snapping is currently a no-op (documented latent
  bug, locked by a test; one-line fix identified).
- Linux-only, by design.

For architecture and testing details see `docs/ARCHITECTURE.md`,
`docs/CURRENT-STATE.md`, and `docs/BUILDING.md`.