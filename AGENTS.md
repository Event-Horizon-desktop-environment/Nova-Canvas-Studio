# AGENTS.md — Nova Canvas Studio

C++20 / Qt6 / FFmpeg **nonlinear video editor** (DaVinci-Resolve-style dark UI). Linux-only. The GUI shell lives in `gui/`, the headless editor/export engine in `core/` (`canvas_core` static lib). Namespaces: everything is `canvas::core` / `canvas::gui` (GPU code is `canvas::core::gpu`).

## Build, run, test

- **Build:** `./build.sh` (also `-d` install deps, `-c` clean, `-t Debug`). Binary → `build/gui/canvas`. It auto-detects distro and picks Ninja.
- **Clean code only, zero warnings:** every build — Debug, Release, and the `build-release/` tree — must be **warning-free**. Treat any compiler warning as a build failure before handing off (or prefer the nearest build that surfaces them, e.g. `-Wall -Wextra -Wunused-result`). No `-Werror` on the GUI app target, so the discipline is manual: never ship a change that warns.
- **Test:** `ctest --test-dir build` — two tests: `roundtrip` (edit-op + project serialization round-trip; run under ASAN) and `export_sweep` (exports every valid codec×container combo to `/tmp/canvas_export_sweep/`; returns 2 = SKIP if no libx264).
- **System deps:** Qt6 (Widgets/OpenGLWidgets/Svg), FFmpeg (libavformat/codec/util/swscale/swresample), nlohmann-json (system header, no explicit CMake dep), optional CUDA 12/13 (`nvcc` → `CANVAS_CUDA_ENABLED`), PipeWire + ALSA (optional, detected via pkg-config).

## Layout

```
AGENTS.md / CMakeLists.txt / build.sh  — build + disambiguation
core/    canvas_core: model, editing, media decode, export (no Qt)
  include/canvas/core/   headers (mirrors src/ tree)
  src/               implementations (+ gpu/cuda_convert.cu)
  tests/             roundtrip_test.cpp, export_sweep_test.cpp
gui/     Qt6 app
  src/UX/            MainWindow, shell, theme, style, logging
  src/features/      MainWindow methods split by feature domain
  src/Widgets/       timeline + viewer widgets
  src/core/timecode.hpp
  ui/MainWindow.ui   designer shell (docks only; rest is programmatic)
  resources.qrc + resources/icons/   SVG icons
third_party/qlementine/   vendored QStyle lib — NOT compiled or included
roadmap.md / ux.md / "plan .md" / n.md   design docs (out of date, skim)
```
`build/`, `*.log` (`canvas_debug.log`, `run_*.log`), `n.md` are transient/disposable.

## core/ — engine (no Qt)

### Timeline model (`timeline/`)
- `include/.../timeline/model.hpp` — THE data model. `Sequence { fps, video_tracks, audio_tracks, bookmarks, next_clip_id }` → `Track { Kind{Video,Audio}, name, locked, clips }` → `Clip { media, tl_in/out, src_in/out, linked_id, enabled, transition_out }` → `MediaId` into `Project::media`. `MediaId=int`, `ClipId=uint64_t`, positions are `int64_t` frames.
- `include/.../timeline/edit_ops.hpp` + `src/timeline/edit_ops.cpp` — undoable edit API. Everything returns `std::unique_ptr<ICommand>` (snapshot-based undo via `TrackSnapshot`): `place_clip/place_linked_clip/unlink_clip/link_clip/lift_range/ripple_delete_range/blade_at/move_clip/set_clip_transition/delete_through_edit` + `UndoStack`. Linked A/V pairs move/delete/transition together.

### Media pipeline (`media/`)
- `frame.hpp` — runtime containers: `VideoFrame` (CPU RGBA + stride), `RenderFrame` (a/b frames + transition progress, `TransitionRenderMode`), `AudioChunk` (float PCM).
- `video_decoder.hpp/.cpp` — FFmpeg decode: HW decode (shared device), CPU RGBA via swscale, low-res preview cap, keyframe-seek vs sequential-forward heuristics (`decode_to_frame`, `seek_to_frame`, `decode_to_hw`, `decode_audio`). Opens a **separate** audio demuxer/seek domain.
- `audio_decoder.hpp/.cpp` — standalone PIMPL audio-only decoder, persistent buffered + resampled.
- `frame_cache.hpp/.cpp` — thread-safe byte-budgeted LRU of decoded `VideoFrame`s (~512MB budget).
- `hw_device.hpp/.cpp` — shared `HwDeviceManager`: probes cuda→vaapi→qsv→vulkan once, falls back to software.
- `audio_waveform.hpp/.cpp` — full-stream scan → per-bucket peak/RMS; `reduce_waveform` re-buckets.

### Project (`project/`)
- `project.hpp/.cpp` — `Project { name, Sequence, media[], bins }`, `MediaEntry { id, path, fps, dims, total_frames, bin }`. JSON save/load, versioned (`kProjectVersion=2`), nlohmann/json.

### Export (`export/`)
- `deliver_preset.hpp/.cpp` — Resolve-style high-level Deliver settings (`DeliverSettings` backward/forward compatible). `to_export_settings()` → low-level `ExportSettings`.
- `exporter.hpp/.cpp` — `export_project()`: the full FFmpeg mux+encode (NVENC/VAAPI/QSV hw encoders, CPU fallback, progress/cancel). `list_video_codecs/list_containers/list_audio_codecs/available_hw_devices`.
- `renderer.hpp/.cpp` — timeline→frames. `render_video_frame` (single), `RenderSession` (reusable across export, persistent decoders), compositing top-down, GPU single-clip fast path (`frame_gpu`, returns borrowed NV12 plane), `render_audio_chunk` mixing.
- `render_queue.hpp/.cpp` — thread-safe background `RenderQueue` (own worker thread) draining `RenderJob`s via `export_project`.

### GPU (`gpu/`)
- `cuda_convert.cu` + `cuda_convert.hpp` — CUDA kernels feeding NVENC directly: `rgbaToNV12` (CPU RGBA→NV12) and `nv12Resize` (GPU NV12→NV12 letterbox resize). Compiled only under `CANVAS_HAVE_CUDA`; all callers guard with `cuda_available()`. BT.601 limited range.

### Util (`util/`)
- `log.hpp` — header-only `CANVAS_LOG(fmt,...)` to stderr + `CANVAS_LOG_FILE` (default `canvas_debug.log`), gated by `CANVAS_DEBUG`.

## gui/ — Qt6 app

- `src/main.cpp` — logging → `QApplication` → theme → `MainWindow`; `window.open_file(argv[1])`.
- `src/UX/MainWindow.hpp/.cpp` — view-controller root class. Owns `SequenceController`, `ThumbnailService`, `ViewerGL`, `TimelineWidget`, docks, `RenderQueue`, `UndoStack`, `Project`. Bridges timeline signals → core edit ops → snapshot → controller.
- `src/UX/MainWindowShell.cpp` (~1100 lines) — ALL programmatic UI chrome (menus, top bar, dock contents, page bar). `ui/MainWindow.ui` only supplies the QMainWindow + 3 docks; shell claims dock corners then populates.
- `src/UX/theme.hpp/.cpp` — dark QSS + palette token set + `SvgIconEngine`. `src/UX/horizon_style.hpp/.cpp` — `HorizonStyle : QProxyStyle` (glassy button/toolbar paint). **qlementine is vendored but unused** — theming is all HorizonStyle + QSS.
- `src/features/` — MainWindow methods split by domain (one class, multiple .cpp):
  - `app/AppActions.cpp` — keyboard, close-event unsaved prompt, clip enable/transition toggles, media placement.
  - `project/ProjectActions.cpp` — import/new/open/save (`*.ehproj`), recent files (QSettings `"recentProjects"`, 10).
  - `timeline/TimelineActions.cpp` — `connect_timeline()`: all timeline widget signals → core ops, undo recording.
  - `playback/sequence_controller.hpp/.cpp` (~870 lines) — the active playback stack: 4 threads (UI/worker/audio-decode/audio-write), 24-frame lookahead `kLookahead`, 640px scrub preview cache, embedded transitions. `playback_controller.hpp/.cpp` — older single-file rendition, largely superseded.
  - `playback/audio_output.hpp/.cpp` — ALSA w/ PipeWire fallback, float PCM, two writer threads, stat counters. Note: `flush()` must join the ALSA thread before dropping (stale-audio bug documented in code).
  - `thumbnails/thumbnail_service.hpp/.cpp` — 4 worker threads, in-memory LRU + disk cache (`cache_dir_`, FNV-1a keys), waveform PNG + raw `.ehwf`.
  - `deliver/deliver_settings_panel.*` + `render_queue_panel.*` — Deliver page UI bound to core `RenderQueue`.
- `src/Widgets/`:
  - `timeline_widget.hpp/.cpp` — `QGraphicsView` facade + geometry constants + signals (everything delegated; no direct model mutation).
  - `timeline_view.cpp` — scene: minimap, ruler, tracks, filmstrip thumbnails, waveform pixmaps, playhead.
  - `timeline_interaction.cpp` — blade, drag/select, snap, transition handle editor, scrub, drops.
  - `timeline_thumbnails.cpp` — thumbnail request/ready state machine.
  - `viewer_gl.hpp/.cpp` — OpenGL preview widget.
  - `media_pool_widget.hpp/.cpp` — media grid; drag uses mime `application/x-eh-media-id`, drops emit `filesDropped`.
- `src/core/timecode.hpp` — `timecode(frame, fps)` → `HH:MM:SS:FF`.

## Key flows

- **Editing:** timeline widget → `TimelineActions` → core edit op (returns `ICommand`) → `UndoStack::record` + `push_snapshot()` (deep copy of Project → passed to `SequenceController` so the worker reads an immutable copy).
- **Playback:** `SequenceController` worker pops commands, decodes via per-clip `VideoDecoder`+, caches, presents on `ViewerGL`; audio pushed to `AudioOutput` (ALSA/PW) while `log_av_sync` reports drift.
- **Export:** Deliver panel → `DeliverSettings` → `to_export_settings()` → `RenderQueue` worker → `export_project()` → `RenderSession` renders frames (CUDA NV12 fast path when possible) → FFmpeg mux.
- **HW decode:** `HwDeviceManager` shared across decoders; `video_decoder` hw-decodes and downloads via `av_hwframe_transfer_data`.

## Gotchas & conventions

- `core/` has **zero Qt** dependency (deliberate); GUI only talks to it via headers.
- Two decoders overlap: `VideoDecoder::decode_audio` vs standalone `AudioDecoder` — know which one a caller uses.
- GPU optional everywhere: guard with `CANVAS_HAVE_CUDA` / `cuda_available()`; never assume NVENC.
- CMake: AUTOMOC/AUTORCC/AUTOUIC on; `CMAKE_AUTOUIC_SEARCH_PATHS ui`; CUDA compiled with `-allow-unsupported-compiler`.
- Timeline tracks are `std::vector<Track>` (not maps); lookups are linear (`clip_at`, `clip_with_id`).
- C++20, no exceptions in the edit path (`std::optional` everywhere); transitions live on the clip itself.
- **Zero-warning discipline (applies to ALL targets, incl. tests and `build-release/`):** never introduce a compiler warning. When a function is `[[nodiscard]]`, consume its result — in tests assign it and `(void)` it or, better, `CHECK` on it; a bare `foo()` statement that discards `[[nodiscard]]` is a bug.
- `build.sh` installs deps via `pkexec`/`sudo` w/ confirmation, prefers Ninja, incremental rebuild unless `-c`.