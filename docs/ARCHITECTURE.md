# Architecture

A deliberately clean split: **a headless engine** (`core/`) that knows
nothing about Qt, and **a Qt GUI shell** (`gui/`) that owns everything to do
with pixels, widgets, and user interaction. The pattern is maintained
religiously — the engine has zero Qt includes, and the most testable parts of
the GUI are extracted into Qt-free modules precisely so they can be verified
headlessly.

```
core/                     canvas_core static lib — editing, media, export
  include/canvas/core/    public headers, mirrors src/ below
  src/                    implementations (+ gpu/cuda_convert.cu)
  tests/                  roundtrip, export_sweep, scrub_bench
gui/                      Qt 6 application
  src/main.cpp            entry point
  src/UX/                 MainWindow, shell chrome, theme, style, logging
  src/features/           MainWindow behavior split by domain
  src/Widgets/            timeline widget + viewer + media pool
  src/core/timecode.hpp   frame → HH:MM:SS:FF
  ui/MainWindow.ui        designer shell (docks only; rest programmatic)
  resources.qrc           SVG icon set
```

No exceptions in the editing path — errors travel through `std::optional`.
Positions in the timeline are `int64_t` frame numbers; `fps` lives on the
`Sequence`.

## core/ — the engine

### Timeline model

`timeline/model.hpp` is THE data model. `Project { name, Sequence, media[],
bins }`; `Sequence { fps, video_tracks, audio_tracks, bookmarks,
next_clip_id }`; `Track { Kind{Video,Audio}, name, locked, clips }`;
`Clip { media, tl_in/out, src_in/out, linked_id, enabled, transition_out }`.
Tracks are `std::vector<Track>`, lookups are linear.

Editing goes through `timeline/edit_ops.hpp` — an undoable edit API where
every operation returns a `std::unique_ptr<ICommand>` and undo works by
snapshotting. Operations include `place_clip`, `place_linked_clip`,
`unlink/link_clip`, `lift_range`, `ripple_delete_range`, `blade_at`,
`move_clip`, `set_clip_transition`, `delete_through_edit`, all backed by
`UndoStack`. Linked A/V pairs move, delete, and transition together.

### Media pipeline

- `frame.hpp` — runtime containers: `VideoFrame` (CPU RGBA + stride),
  `RenderFrame` (a/b frames + transition progress), `AudioChunk` (float PCM).
- `video_decoder` — FFmpeg decoding. Hardware decode through a shared device,
  CPU RGBA via swscale, a low-res preview cap, and keyframe-seek vs
  sequential-forward heuristics. Opens a *separate* audio demuxer/seek domain.
- `audio_decoder` — standalone PIMPL audio decoder, persistent, buffered,
  resampled. Note: it overlaps with `VideoDecoder::decode_audio`; a caller
  picks one.
- `frame_cache` — thread-safe, byte-budgeted LRU (~512 MB) of decoded frames.
- `hw_device` — a shared `HwDeviceManager` probing cuda → vaapi → qsv →
  vulkan once, falling back to software.
- `audio_waveform` — full-stream scan producing per-bucket peak/RMS; re-buckets
  via `reduce_waveform`.

### Project, export, GPU

- `project/` — `Project` JSON save/load, versioned (`kProjectVersion`). Files
  use the `.ehproj` extension; the JSON key is `canvas_project` (the old
  `event_horizon_project` key still reads fine).
- `export/` — `deliver_preset` holds the high-level Deliver settings model
  mapped down to low-level `ExportSettings`; `exporter` runs the FFmpeg
  mux+encode (NVENC/VAAPI/QSV hardware encoders, CPU fallback, progress +
  cancel); `renderer` turns timeline → frames (top-down compositing, a GPU
  single-clip fast path returning borrowed NV12 planes, `render_audio_chunk`
  mixing); `render_queue` runs exports on a background worker thread.
- `gpu/` — CUDA kernels feeding NVENC directly: `rgbaToNV12` (CPU RGBA →
  NV12) and `nv12Resize` (letterbox resize). Compiled only when `CANVAS_HAVE_CUDA`;
  callers guard with `cuda_available()`. BT.601 limited range.
- `util/log.hpp` — header-only `CANVAS_LOG(fmt, …)` → stderr + log file
  (default `canvas_debug.log`), gated by `CANVAS_DEBUG`.

## gui/ — the Qt shell

`MainWindow` is the view-controller root: it owns the `SequenceController`,
`ThumbnailService`, `ViewerGL`, `TimelineWidget`, docks, render queue, undo
stack, and the `Project`, and it bridges timeline signals → core edit ops →
snapshot → controller. `MainWindowShell.cpp` builds ALL programmatic UI chrome
(menus, top bar, docks, page bar); `MainWindow.ui` only supplies the window +
three docks.

Behavior is split across `src/features/` as one class in several files:

- `app/AppActions.cpp` — keyboard, close-event unsaved prompt, clip
  enable/transition toggles, media placement.
- `project/ProjectActions.cpp` — import/new/open/save, recent files
  (QSettings, 10 entries).
- `timeline/TimelineActions.cpp` — `connect_timeline()`: every timeline
  widget signal → core op with undo recording.
- `playback/sequence_controller.*` — the active playback stack: four threads
  (UI/worker/audio-decode/audio-write), 24-frame lookahead, 640px scrub
  preview cache, embedded transitions. (`playback_controller.*` is the older
  rendition, largely superseded.)
- `playback/audio_output.*` — ALSA with PipeWire fallback, float PCM, two
  writer threads. Known quirk documented in code: `flush()` must join the ALSA
  thread before dropping to avoid stale audio.
- `thumbnails/thumbnail_service.*` — 4 worker threads, in-memory LRU + disk
  cache (FNV-1a keys), waveform PNGs + raw `.ehwf`.
- `deliver/` — Deliver page UI bound to the core `RenderQueue`.

### The timeline widget

A `QGraphicsView` facade that owns the scene but does not mutate the model
directly — it emits signals and lets `TimelineActions` do the editing:

- `timeline_widget.*` — facade + geometry constants + signals
- `timeline_view.cpp` — scene drawing: minimap, ruler, timecode bar, tracks,
  filmstrip thumbnails, waveform pixmaps, playhead
- `timeline_interaction.cpp` — blade, drag/select, snap, transition handle
  editor, scrub (ruler-click seek + playhead drag), drag-and-drop
- `timeline_thumbnails.cpp` — thumbnail request/ready state machine
- `timeline_snap.cpp`, `timeline_selection.cpp`, `timeline_drag.cpp` —
  extracted Qt-free logic, tested headlessly

Visual details worth knowing: the top strip (timecode bar / minimap / ruler)
and its children are *pinned* so they don't scroll, and they're mouse-
transparent so scrubbing works over them; the playhead draws on top. The
divider band between channels is the pan grip — grab it to pull the channels
down into the scroll room; pulling up clamps at the limit.

### Theming

`theme.*` provides the dark QSS palette + an `SvgIconEngine`;
`horizon_style.*` (`HorizonStyle : QProxyStyle`) paints the glassy
buttons/toolbars. All theming is custom — no third-party style library.

## Key flows end to end

**Editing** — widget → `TimelineActions` → core edit op (`ICommand`) →
`UndoStack::record` + `push_snapshot()` (a deep copy of the project handed to
the controller so its worker reads an immutable snapshot).

**Playback** — the controller's worker pops commands, decodes via per-clip
`VideoDecoder`s (cached), presents on the `ViewerGL`; audio is pushed to
`AudioOutput` while `log_av_sync` reports drift.

**Export** — Deliver panel → `DeliverSettings` → `to_export_settings()` →
`RenderQueue` worker → `export_project()` → `RenderSession` renders frames
(CUDA NV12 fast path when possible) → FFmpeg mux.

**Hardware decode** — one shared `HwDeviceManager`; `video_decoder`
hw-decodes and downloads with `av_hwframe_transfer_data`.

## Conventions that matter if you touch the code

- **`core/` never includes Qt.** GUI code reaches the engine only through
  headers. Breaking this breaks the headless-seam tests by design.
- **Two decoders overlap** (`VideoDecoder::decode_audio` vs `AudioDecoder`) —
  make sure you know which one a new caller should use.
- **GPU is optional everywhere.** Guard with `CANVAS_HAVE_CUDA` /
  `cuda_available()`; never assume NVENC exists.
- **CMake:** AUTOMOC/AUTORCC/AUTOUIC on, `CMAKE_AUTOUIC_SEARCH_PATHS ui`,
  CUDA compiled with `-allow-unsupported-compiler`.
- **Transitions live on the clip**, not in a separate lane.
- **Zero warnings**, always, in every build tree and every test target.