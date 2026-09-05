# Current State

Honest status as of 2026-09-05. This is an **alpha-grade** nonlinear video
editor. A lot works, some things are rough, and a few known issues are parked
with clear explanations.

## At a glance

- **Name / build identity:** Nova Canvas Studio, binary `canvas`. Renamed on
  2026-09-05 from "Event Horizon Studio" / `event-horizon` — namespace
  `canvas::`, include dir `core/include/canvas/`, lib `canvas_core`, project
  file key `canvas_project` (legacy `event_horizon_project` still reads).
- **Version:** 0.1.0, C++20, Linux only.
- **Builds:** clean (zero warnings) in Debug and Release into `./build`.
- **Tests:** 11 in CTest — 8 GUI-headless + 3 core; 10 pass, 1 known-fail.

## What works today

**The editor core**
- Undoable editing: place/unlink/link/lift/ripple-delete/blade/move/transition/
  delete-through-edit, via a snapshot-based command stack
- Linked A/V clip pairs move, delete, and transition together
- Timeline model (de)serialization to `.ehproj` (JSON), versioned

**The timeline UI**
- Dark, tool-grade UI with pinned top strip: live timecode readout,
  minimap, ruler with tick labels
- Playhead drawn over the ruler/grid; ruler-click seek and playhead drag
- Divider band between the channel groups: grab it to pan the channels —
  pull **up** clamps at the limit (the channels' resting seat under the
  ruler), pull **down** sinks them through a deep scroll room
- Clip drag with snap, zoom-dependent quantization, selection model with
  linked-mate coalescing, V1/A1 resize edges on the band
- Filmstrip thumbnails + waveform previews (async thumbnail service)

**Media & playback**
- FFmpeg decode with shared hardware decode device (cuda → vaapi → qsv →
  vulkan, falling back to software), frame cache, low-res preview cap
- 4-thread playback stack with 24-frame lookahead, scrub preview cache,
  embedded transitions, ALSA/PipeWire audio output, A/V sync logging

**Export**
- Deliver-panel style settings, NLE-style codec/container lists
- NVENC/VAAPI/QSV hardware encoders with CPU fallback, cancel + progress
- Background render queue; GPU NV12 fast path when CUDA is available

**Testing infrastructure**
- Headless Qt-free harness: core engine + extracted GUI logic compile and run
  with no Qt and no display (enforced at build time)

## Recent changes (2026-09-05)

- **Full rename to Nova Canvas Studio** — code, CMake, packaging, app
  identity. Legacy `.ehproj` files still open; old events in system menus are
  cleaned up by `just uninstall`.
- **Timeline top strip reworked** — added the pinned timecode bar, moved rack
  geometry around it, fixed scrub hit-testing to viewport coordinates, pinned
  strip children made mouse-transparent, playhead raised to draw over the
  ruler/grid.
- **Divider-band pan semantics finalized** — grab-and-follow (not
  scrollbar-inverted): up is clamped at the parked limit, down empties into
  the void.

## Known issues & quirks

- **`roundtrip` test SEGFAULTs** — pre-existing, unrelated to recent work. It
  is the oldest test in the repo, written against an earlier timeline model,
  and still trips a stale path. The rest of the suite is green. It's
  scheduled for a rewrite against `edit_ops` + current model (run under ASan).
- **Two decoders overlap** — `VideoDecoder::decode_audio` vs the standalone
  `AudioDecoder`. Both exist; callers choose one. Refactoring to a single
  audio path is on the list.
- **Stale-audio flush quirk** — `AudioOutput::flush()` must join the ALSA
  writer thread before being dropped; this is documented in the code and
  worked around at call sites.
- **GPU is a build-time bonus, not a requirement.** Without CUDA you get CPU
  encoding; without ALSA/PipeWire you get no audio but still full video.
- **Automated GUI testing is limited** — offscreen `QOpenGLWidget` probes
  segfault, so the viewer/timeline interaction layer is verified by launching
  the real app.

## Where the docs diverge from reality

Some older markdown notes (the root design documents — `roadmap.md`,
`ux.md`, `plan .md`, `n.md`, `projectmap.md`) are historical design docs for
the pre-rename "Event Horizon" project. Skim for intent, don't trust them for
details. This `docs/` folder and `AGENTS.md` are current. The `AGENTS.md`
file still says "two tests"; the real count is eleven.

## Sensible next steps

- Rewrite/fix the `roundtrip` test against the current model
- Unify the two audio decode paths
- Continue the extraction-then-test pattern for GUI logic (it's working well)
- Keep the zero-warning + no-Qt-in-core discipline on every change