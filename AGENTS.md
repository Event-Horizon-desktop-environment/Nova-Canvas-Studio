# AGENTS.md — Nova Canvas Studio

C++20 / Qt6 / FFmpeg **nonlinear video editor** (dark, editor-grade UI). Linux-only. The GUI shell lives in `gui/`, the headless editor/export engine in `core/` (`canvas_core` static lib). Namespaces: `canvas::core` / `canvas::gui` (GPU code is `canvas::core::gpu`).

Renamed from **Event Horizon Studio** to **Nova Canvas Studio** on 2026-09-05 (binary `canvas`). Project files write the `canvas_project` key; older files with the legacy `event_horizon_project` key still load.

Mid-refactor: `splitplan.md` is a 40-phase plan to split the codebase into small, Qt-free-where-possible, independently-testable modules. **Phases 1–30 are done** (headless extraction of the playback stack + timeline interaction, see below); phases 31–40 (Deliver-panel split, audio-path dedup, dead-code removal, `-Werror` hardening, this file) are still open. Check `splitplan.md`'s checkbox state before assuming a module described below hasn't moved again.

## Build, run, test

- **Build:** `./build.sh` (also `-d` install deps, `-c` clean, `-t Debug`). Binary → `build/gui/canvas`. Auto-detects distro, prefers Ninja.
- **Clean code only, zero warnings:** every build — Debug, Release, `build-release/` — must be warning-free. Treat any compiler warning as a build failure before handing off. No `-Werror` on the GUI app target (only on the headless module targets, since Phase 21/38), so discipline on the GUI side is manual.
- **Test:** `ctest --test-dir build`. **13 tests** (verified passing on this machine):
  - **Core (4, in `core/tests/`):** `roundtrip` (edit-op + project JSON round-trip; an edge in this test used to SEGFAULT on a stale timeline model until `EqBand::operator==` was added — it now passes), `export_sweep` (every valid codec×container combo; skip=2 if no libx264), `scrub_bench` (p95 scrub-preview latency budget; skip=2 if no libx264), `visual_render_test` (eye: identity video render is byte-identical to the legacy fast path, plus flip/scale/opacity actually change output pixels; synthesizes its own h264 source at runtime).
  - **GUI-headless (9, in `gui/tests/`, registered in `gui/tests/CMakeLists.txt`):** `sync_constants_test`, `timeline_decoder_test`, `audio_pipeline_test`, `sonicsync_test`, `av_reanchor_test` (seek-hold vs. audio-feed integration), `timeline_snap_test`, `timeline_selection_test`, `timeline_drag_test`, `transition_handle_editor_test`.
  - If you quote a test count elsewhere, assume these 13 and re-verify with `ctest --test-dir build` before quoting.
- **`scripts/check_qtdep.sh`** — static guard for the headless invariant below (`-q` for exit-code-only, used by CI/`build.sh`). Also enforced at compile time: `canvas_add_headless_test()` targets link no Qt, so a stray `<Q...>` include fails the build outright.
- **System deps:** Qt6 (Widgets/OpenGLWidgets/Svg), FFmpeg (libavformat/codec/util/swscale/swresample), nlohmann-json (system header, no explicit CMake dep), optional CUDA 12/13 (`nvcc` → `CANVAS_HAVE_CUDA`), PipeWire + ALSA (optional, pkg-config-detected).

## Headless invariant (read before touching anything under `gui/src/features/playback/` or `gui/src/Widgets/`)

> A headless module may include ONLY `<system>`, `<canvas/core/...>`, and other headless modules. Never `<Q...>`. If a module needs Qt, it is NOT headless and needs a thin Qt adapter around it instead.

The current headless surface (besides all of `core/`): `sync_constants.hpp`, `audio_sink.hpp`, `audio_pipeline.hpp/.cpp`, `sonicsync.hpp/.cpp`, `timeline_decoder.hpp/.cpp` (all in `gui/src/features/playback/`), and `timeline_snap.hpp/.cpp`, `timeline_selection.hpp/.cpp`, `timeline_drag.hpp/.cpp`, `transition_handle_editor.hpp/.cpp` (all in `gui/src/Widgets/`). Run `./scripts/check_qtdep.sh` after touching any of these.

**FROZEN APIs (splitplan Phase 22):** `TimelineDecoder`, `AudioPipeline`, and `SonicSync`'s public surfaces are the stable playback seam. Don't change existing signatures without checking `splitplan.md`'s sign-off note; additive methods are fine.

## Layout

```
AGENTS.md / CMakeLists.txt / build.sh          — build + disambiguation
core/    canvas_core: model, editing, media decode, export (no Qt)
  include/canvas/core/   headers (mirrors src/ tree)
    timeline/            model.hpp, edit_ops.hpp, audio_fade.hpp, audio_mix.hpp, visual.hpp
  src/                   implementations (+ gpu/cuda_convert.cu)
  tests/                 roundtrip_test.cpp, export_sweep_test.cpp, scrub_bench_test.cpp,
                         visual_render_test.cpp (wired + passing)
gui/     Qt6 app
  src/UX/                MainWindow + the split shell builders (see below), theme, style, logging,
                         InspectorVisual/Audio/Transition/File (categorical inspector pages)
  src/features/          MainWindow methods split by feature domain
    playback/            SequenceController (thin coordinator) + the extracted headless modules
  src/Widgets/           timeline + viewer widgets, incl. the extracted interaction modules
  src/core/timecode.hpp
  ui/MainWindow.ui       designer shell (docks only; rest is programmatic)
  resources.qrc + resources/icons/   SVG icons
  tests/                 gui-side headless unit tests (see Build/test above)
scripts/
  check_qtdep.sh         static no-Qt-in-headless-modules guard
  split-snapshot.sh      pre-phase backup utility for the refactor
splitplan.md             the 40-phase refactor plan + running log (source of truth for phase status)
docs/                    ARCHITECTURE.md, BUILDING.md, DEPENDENCIES.md, CURRENT-STATE.md — current
roadmap.md / ux.md / "plan .md" / n.md   pre-rename design docs — historical, skim for intent only
```
`build/`, `*.log` (`canvas_debug.log`, `run_*.log`) are transient/disposable.

## core/ — engine (no Qt)

### Timeline model (`timeline/`)
- `model.hpp` — THE data model. `Sequence { fps, video_tracks, audio_tracks, bookmarks, next_clip_id }` → `Track { Kind{Video,Audio}, name, locked, solo, clips }` → `Clip { media, tl_in/out, src_in/out, linked_id, enabled, transition_out/in, volume_db, pan, scale_x/y, pos_x/y, rotation_deg, anchor_dx/dy, opacity, blend_mode }` → `MediaId` into `Project::media`. `MediaId=int`, `ClipId=uint64_t`, positions are `int64_t` frames. `Track::solo` and the per-clip audio-mix (`volume_db`, `pan`) and visual-transform/composite fields back the Inspector's Audio and Video tabs — see `audio_mix.hpp`/`visual.hpp` below for the shared ranges/laws.
- `edit_ops.hpp` + `edit_ops.cpp` — undoable edit API. Everything returns `std::unique_ptr<ICommand>` (snapshot-based undo via `TrackSnapshot`): `place_clip/place_linked_clip/unlink_clip/link_clip/lift_range/ripple_delete_range/blade_at/move_clip/set_clip_transition(_in)/delete_through_edit` + `UndoStack`. Linked A/V pairs move/delete/transition together.
- `audio_fade.hpp` — Qt-free audio-transition gain envelopes (`audio_fade_gain(clip, tl_frame)`). Renders the audible ramp for `AudioFadeConstantGain/Exponential/ConstantPower` IN/OUT windows; ignores video-only transition types. Single-clip ramps only — no two-clip overlap crossfade math.
- `audio_mix.hpp` — per-clip volume/pan laws shared by playback (`AudioPipeline`), export (`renderer.cpp`), and the Inspector, so the gain math is identical everywhere: `db_to_gain` (dBFS → linear, floor at `kMinVolumeDb = -60`, ceiling `kMaxVolumeDb = +24`), `pan_gains` (linear stereo BALANCE law: pan 0 = both channels unity, moving the control rides the opposite channel down), `any_solo(tracks)`.
- `visual.hpp` — shared numeric ranges for the per-clip Transform/Composite properties (Zoom 0..10, Position/Anchor ±4096 px, Rotation ±360°, Opacity 0..1, 5 blend modes) so `edit_ops` clamping and the Inspector's spin boxes never diverge.

### Media pipeline (`media/`)
- `frame.hpp` — runtime containers: `VideoFrame` (CPU RGBA + stride), `Nv12Frame` (GPU composite path), `RenderFrame` (a/b frames + transition progress, `TransitionRenderMode`), `AudioChunk` (float PCM).
- `video_decoder.hpp/.cpp` — FFmpeg decode: HW decode (shared device), CPU RGBA via swscale, low-res preview cap, keyframe-seek vs sequential-forward heuristics (`decode_to_frame`, `seek_to_frame`, `decode_to_hw`, `decode_audio`). Opens a **separate** audio demuxer/seek domain from the video one.
- `audio_decoder.hpp/.cpp` — standalone PIMPL audio-only decoder, persistent buffered + resampled.
- `frame_cache.hpp/.cpp` — thread-safe byte-budgeted LRU of decoded `VideoFrame`s (~512MB budget).
- `hw_device.hpp/.cpp` — shared `HwDeviceManager`: probes cuda→vaapi→qsv→vulkan once, falls back to software.
- `audio_waveform.hpp/.cpp` — full-stream scan → per-bucket peak/RMS; `reduce_waveform` re-buckets cheaply.

### Project (`project/`)
- `project.hpp/.cpp` — `Project { name, Sequence, media[], bins }`, `MediaEntry { id, path, fps, dims, total_frames, bin }`. JSON save/load, `kProjectVersion = 3`. **Writes the `"canvas_project"` key**; on load, checks `"canvas_project"` first and falls back to the legacy `"event_horizon_project"` key for older files. Rejects files with a version newer than 3.

### Export (`export/`)
- `deliver_preset.hpp/.cpp` — high-level Deliver settings (`DeliverSettings`). `to_export_settings()` → low-level `ExportSettings`.
- `exporter.hpp/.cpp` — `export_project()`: the full FFmpeg mux+encode (NVENC/VAAPI/QSV hw encoders, CPU fallback, SFE + NVENC level stamping, progress/cancel). `list_video_codecs/list_containers/list_audio_codecs/available_hw_devices`.
- `renderer.hpp/.cpp` — timeline→frames. `render_video_frame` (single-shot), `RenderSession` (reusable across export, persistent decoders), compositing top-down (now applying per-clip volume/pan/transform/blend/opacity, not just raw blit), GPU single-clip fast path (`frame_gpu`), `render_audio_chunk` mixing.
- `render_queue.hpp/.cpp` — thread-safe background `RenderQueue` (own worker thread) draining `RenderJob`s via `export_project`.

### GPU (`gpu/`)
- `colorspace.hpp` — shared BT.601 `yuv_to_rgb`/`rgb_to_yuv`, single source of truth for the viewer's CPU fallback and the CUDA kernel's documented contract.
- `cuda_convert.cu`/`.hpp` — CUDA kernels feeding NVENC directly: `rgbaToNV12`, `nv12Resize`. Compiled only under `CANVAS_HAVE_CUDA`; all callers guard with `cuda_available()`.

### Util (`util/`)
- `log.hpp` — header-only `CANVAS_LOG(fmt,...)` (env-gated by `CANVAS_DEBUG`) + unconditional `log_error`/`log_warning`.

## gui/ — Qt6 app

- `src/main.cpp` — logging → `QApplication` → theme → `MainWindow` → `window.open_file(argv[1])`.
- `src/UX/MainWindow.hpp/.cpp` — view-controller root class. Owns `SequenceController`, `ThumbnailService`, `ViewerGL`, `TimelineWidget`, docks, `RenderQueue`, `UndoStack`, `Project`. Deliver-page actions (enter/exit + render-queue ops) now live in `features/deliver/DeliverActions.cpp`.
- `src/UX/MainWindowShell.cpp` — now just a **~40-line ordered coordinator** (`build_ui()`) that calls into the split builders below; it used to hold ~1200 lines of chrome directly (splitplan Phases 23–26 moved it out — don't be surprised this file is tiny now):
  - `ShellMenus.cpp` — `build_app_menus`: File/Edit/Trim/Timeline/Clip/Mark/View/Playback + stub menus.
  - `ShellTopBar.cpp` / `ShellPageBar.cpp` / `ShellTransportBar.cpp` — the top status strip (`build_top_bar`), the page-switcher toolbar (`build_page_bar`), and the playback transport bar (`build_transport_bar`, which pulls in `Widgets/viewport_selector.hpp` for the hand-painted timebase selector).
  - `ShellMediaDock.cpp` — `build_left_dock` (bins tree + media pool + placeholder tabs + Resolve-style collapse sliver).
  - `ShellInspectorDock.cpp` — `build_inspector_dock` (mode pills + the `InspectorCategory` scroll + page stack; the pages themselves build in `InspectorVisual`/`InspectorAudio`/`InspectorTransition`/`InspectorFile`).
  - `ShellCenter.cpp` — `build_center_workspace`: viewer column, timeline dock.
  - `ShellDeliverPage.cpp` — `build_deliver_docks`: Deliver-settings + render-queue docks, their signal plumbing, and the job-failure dialogs.
- `src/UX/InspectorShared.hpp` — the shared `InspectorCategory` collapsible-group widget + `add_property_row`/`make_numeric` helpers, extracted out of the old `ShellDocks.cpp` so `InspectorVisual` doesn't duplicate them.
- `src/UX/InspectorVisual.hpp/.cpp` — builds and wires the Inspector's Video-tab Transform/Composite categories against the selected clip's model fields (`build_inspector_visual`, `attach_inspector_visual`, `update_inspector_visual`, `apply_inspector_visual`). Each edit commits as one undoable command via the normal `edit_ops` path.
<<<<<<< Updated upstream
- `src/UX/theme.hpp/.cpp` — dark QSS + palette token set + `SvgIconEngine`. `src/UX/horizon_style.hpp/.cpp` — `HorizonStyle : QProxyStyle` glassy button/toolbar paint.
=======
- `src/UX/InspectorAudioEq.hpp/.cpp` — the interactive EQ response-graph widget (`EqGraphWidget`), extracted out of `InspectorAudio.cpp`. Hand-painted log-frequency plot of the true 6-band cascade magnitude (the shared `canvas::core::equalizer_response` law); `Curve` mode is a draggable-node graph engine, `Bands` mode a row of gain faders. Live edits stream via `on_edit`, settled gestures commit once via `on_commit`.
- `src/UX/InspectorAudio.cpp` — the Inspector's Audio tab (Volume/Pan, Pitch, Speed Change, **interactive EQ** via `InspectorAudioEq`'s `EqGraphWidget`, AI Voice Isolation). Callbacks are `std::function` (no signals/moc).
- `src/UX/theme.hpp` — umbrella header over the split theme modules: `theme_tokens.hpp/.cpp` (design tokens + `tokens()`/`css()`), `theme_state.hpp/.cpp` (dark/light mode, `apply_theme`, re-apply callbacks, panel shadows), `theme_clip_colors.hpp/.cpp` (Resolve-style clip swatches), `theme_icons.hpp/.cpp` (SVG tinting via `SvgIconEngine`), `theme_menu.hpp/.cpp` (rounded popup cards), `theme_styles.hpp/.cpp` (per-widget chrome QSS). `src/UX/horizon_style.hpp/.cpp` — `HorizonStyle : QProxyStyle` glassy button/toolbar paint.
>>>>>>> Stashed changes
- `src/features/`:
  - `app/AppActions.cpp` — keyboard, close-event unsaved prompt, clip enable/transition toggles, media placement.
  - `project/ProjectActions.cpp` — import/new/open/save (`*.ehproj`, legacy extension name kept), recent files (QSettings `"recentProjects"`, cap 10).
  - `timeline/TimelineActions.cpp` — `connect_timeline()`: every timeline-widget signal → core edit op, undo recording.
  - `playback/sequence_controller.hpp/.cpp` — **now a thin coordinator**, not the ~1770-line monolith this file used to describe. It owns the command queue/worker thread and composes `TimelineDecoder` (video decode/composite), `AudioPipeline` (audio decode/feed/A-V-sync bookkeeping, talks to the device only through the abstract `AudioSink`), and `SonicSync` (drop-to-realtime cap policy). `playback_controller.hpp/.cpp` — older single-file rendition, still compiled but superseded; slated for removal (splitplan Phase 34).
  - `playback/timeline_decoder.hpp/.cpp` — **headless.** Owns the per-media `VideoDecoder`+`FrameCache` slots, the low-res scrub-preview LRU, the shared `HwDeviceManager`, and the GPU NV12 fast path. Public surface: `add_media`, `close`, `invalidate`, `decode`, `decode_nv12`, `frame`, `preview`, `media_rate_at`, `make_black_frame`, `hw`, `is_loaded`/`is_hardware`. FROZEN API.
  - `playback/audio_pipeline.hpp/.cpp` — **headless.** Owns the per-media `AudioDecoder` set, playhead↔sample math, A/V-sync run anchors, feed watermarks, scrub-audio grains/reposition feed, and the mixed-source write path (now applying per-clip volume/pan via `audio_mix.hpp` and the fade envelopes via `audio_fade.hpp` before summing). Talks to the device only through `AudioSink` — never a concrete `AudioOutput` — so it can run against a fake sink in tests. FROZEN API.
  - `playback/audio_sink.hpp` — **headless.** Abstract interface (`open/close/write_float/flush/reposition_enqueue/audible_position_frames/...`) that `AudioOutput` implements 1:1; exists purely so `AudioPipeline` never has to see ALSA/PipeWire.
  - `playback/sonicsync.hpp/.cpp` — **headless.** A/V sync drop-to-realtime cap policy (`reconcile`), unchanged in spirit from before the split, now living alongside its siblings. FROZEN API.
  - `playback/sync_constants.hpp` — **headless.** `kLookahead`, `kScrubPrecache`, `kPreviewMaxDim`, `kCommitSeqMaxDelta`, `kAudioLeadMs`.
  - `playback/audio_output.hpp/.cpp` — ALSA w/ PipeWire fallback, float PCM, two writer threads, stat counters. `flush()` must join the ALSA thread before dropping (documented stale-audio bug/workaround).
<<<<<<< Updated upstream
  - `thumbnails/thumbnail_service.hpp/.cpp` — 4 worker threads, in-memory LRU + disk cache, waveform PNG + raw `.ehwf`.
  - `deliver/deliver_settings_panel.*` + `render_queue_panel.*` — Deliver page UI bound to core `RenderQueue`. Splitting the codec/container list builders out into a Qt-free `DeliverSettingsModel` is splitplan Phase 31 — **not done yet**.
=======
  - `thumbnails/thumbnail_service.hpp/.cpp` — 4 worker threads, in-memory LRU + disk cache, waveform PNG + raw `.ehwf`. Decodes thumbnail frames at the `kPreviewMaxDim` (640) preview cap so zoom-out filmstrip rebuilds (hundreds of cells) stream in at ms cost instead of full-res decodes.
  - `deliver/deliver_settings_panel.*` — Deliver page UI wiring only: combo population, signal
    plumbing, `settings()`/`set_settings()` round-trip against the model. Its codec/container
    lists now live in the **headless** `deliver_settings_model.*` (splitplan Phase 31, done):
    `preset_names()`, `encoder_backends()`, `video_codecs_for_format()`/`audio_codecs_for_format()`,
    `bitrate_visibility()`. `render_queue_panel.*` — render-queue UI bound to core `RenderQueue`.
    `DeliverActions.cpp` — `enter_deliver_page()`/`enter_edit_page()`, queue reflection, and the
    add-to-queue / render-all actions extracted out of `MainWindow.cpp` (splitplan refactor).
>>>>>>> Stashed changes
- `src/Widgets/`:
  - `timeline_widget.hpp/.cpp` — `QGraphicsView` facade + geometry constants + signals. Still ~530 lines (Phase 32's "drop dead members, get under ~150 lines" cleanup hasn't run yet — don't assume it's a thin facade until that phase lands).
  - `timeline_view.cpp` — scene: minimap, ruler, tracks, filmstrip thumbnails, waveform pixmaps, playhead.
  - `timeline_interaction.cpp` — mouse/drag/blade/select/transition/scrub/drop handling; delegates the extracted math below rather than doing it inline. Grew rather than shrank across the split (now ~1400 lines) — it picked up new interaction surface (e.g. the viewport selector wiring) even as pure math moved out, so don't use line count alone as a split-progress signal here.
  - `timeline_drag.hpp/.cpp` — **headless.** `timeline_drag::DragController`: clip-drag session state (grab-frame offset frozen at press, same-kind-track targeting, snap-aware commit decision).
  - `timeline_selection.hpp/.cpp` — **headless.** `timeline_selection`: range-membership queries + linked-mate expansion + an owning `SelectionState`.
  - `timeline_snap.hpp/.cpp` — **headless.** `timeline_snap::grid_step`/`snap_to_grid`: zoom-dependent power-of-two grid quantization.
  - `transition_handle_editor.hpp/.cpp` — **headless.** `transition_editor::Editor`: the full transition-handle drag session (open/seed/clamp, begin/move/end drag, favourite-preset snap). Known latent bug, left in place on purpose: the preset-snap loop seeds `best = dur` instead of `INT64_MAX`, so no preset ever beats the live duration — snapping is currently a no-op. One-line fix (`best = INT64_MAX`) is a good small follow-up; don't "fix" it silently inside an unrelated change, since the current behavior is locked by a test that encodes it.
  - `viewer_gl.hpp/.cpp` — OpenGL preview widget, transition shader.
  - `viewport_selector.hpp/.cpp` — hand-painted (no QComboBox) transport-bar timebase selector; paints its own popup so nothing draws over the label/chevron.
  - `media_pool_widget.hpp/.cpp` — media grid; drag mime `application/x-eh-media-id` (legacy mime string kept), drops emit `filesDropped`.
- `src/core/timecode.hpp` — `timecode(frame, fps)` → `HH:MM:SS:FF`.

## Key flows

- **Editing:** timeline widget → `TimelineActions` → core edit op (returns `ICommand`) → `UndoStack::record` + `push_snapshot()` (deep copy of Project → passed to `SequenceController` so the worker reads an immutable copy).
- **Playback:** `SequenceController` worker pops commands, delegates video to `TimelineDecoder`, audio to `AudioPipeline` (which mixes per-clip volume/pan/fades before writing), presents on `ViewerGL`; `SonicSync` caps how far video can run ahead of the audible position.
- **Export:** Deliver panel → `DeliverSettings` → `to_export_settings()` → `RenderQueue` worker → `export_project()` → `RenderSession` renders frames (now including per-clip mix/transform/composite, CUDA NV12 fast path when possible) → FFmpeg mux.
- **HW decode:** `HwDeviceManager` (now owned by `TimelineDecoder`, shared across its decoder slots); `video_decoder` hw-decodes and downloads via `av_hwframe_transfer_data`, or hands back a borrowed device plane for the GPU composite path.

## Gotchas & conventions

- `core/` has **zero Qt** dependency (deliberate); so do the extracted `gui/src/features/playback/*` and `gui/src/Widgets/timeline_{snap,selection,drag}.*` + `transition_handle_editor.*` modules listed above — run `./scripts/check_qtdep.sh` after touching any of them.
- Two decoders overlap: `VideoDecoder::decode_audio` vs standalone `AudioDecoder` — know which one a caller uses. Playback uses `AudioDecoder` (via `AudioPipeline`); de-duplicating this is splitplan Phase 33, **not done**.
- GPU optional everywhere: guard with `CANVAS_HAVE_CUDA` / `cuda_available()`; never assume NVENC.
- CMake: AUTOMOC/AUTORCC/AUTOUIC on for the GUI target; off for `canvas_core` and the headless test targets. `-Werror` is on for headless module targets only (Phase 38), not the GUI app — manual discipline there.
- Timeline tracks are `std::vector<Track>` (not maps); lookups are linear (`clip_at`, `clip_with_id`).
- C++20, no exceptions in the edit path (`std::optional` everywhere); transitions live on the clip itself.
- Per-clip audio mix and visual transform now live directly on `Clip` (`volume_db`, `pan`, `scale_x/y`, `pos_x/y`, `rotation_deg`, `anchor_dx/dy`, `opacity`, `blend_mode`) — always route new gain/transform math through `audio_mix.hpp`/`visual.hpp` rather than re-deriving the laws locally, so playback/export/Inspector never drift.
- **Zero-warning discipline (all targets, incl. tests and `build-release/`):** never introduce a compiler warning. Consume `[[nodiscard]]` results deliberately (assign + `(void)`, or better, `CHECK` on it).
- `build.sh` installs deps via `pkexec`/`sudo` w/ confirmation, prefers Ninja, incremental rebuild unless `-c`.
- **This file drifts.** It was stale for months before this rewrite (wrong test count, wrong project-file key, pre-split module layout). Treat `splitplan.md`'s checkbox state and `docs/CURRENT-STATE.md` as more current than any prose paragraph here that isn't cross-checked against actual source — verify against the real files before trusting a specific line count, test count, or "not yet done" claim in this document once more time has passed.
