# Nova Canvas Studio

A Linux-only, source-built **nonlinear video editor** with a dark, tool-grade
flavor: a proper timeline with a pinned ruler/minimap/timecode strip,
linked A/V editing, hardware-accelerated decode and encode, and a headless
engine tested separately from the GUI.

C++20, Qt 6, FFmpeg. No network builds, no vendored code in the build — it
compiles from your system packages.

## Status

Alpha, very much in motion. As of 2026-09-05 the app builds warning-free, 10
of 11 tests pass, the timeline top strip + divider-band pan were just
reworked, and the project was renamed (from Event Horizon Studio) to **Nova
Canvas Studio** — binary `canvas`, project files still read the legacy
format. Details, honesty included, live in [Current State](docs/CURRENT-STATE.md).

## Quick start

```sh
./build.sh                 # build → ./build/gui/canvas
./build/gui/canvas         # run
ctest --test-dir build     # tests
```

`./build.sh -d` installs dependencies through your package manager
(Fedora/Arch/Debian/Ubuntu/PikaOS) after confirming. More in
[Building](docs/BUILDING.md).

## What you get

- Undoable editing with linked A/V pairs, transitions, snapping, ripple
  delete, blade, and a snapshot-based command stack
- A timeline that feels like the tools you already know: pinned ruler +
  minimap + live timecode bar, playhead over the grid, scrub by clicking the
  ruler or dragging the playhead, and a divider band that pans the channels
  (pull up = hit the limit, pull down = slide into the void)
- FFmpeg decode/encode with hardware paths when available (NVENC/VAAPI/QSV,
  plus a CUDA fast path if you have a toolkit), ALSA/PipeWire audio,
  background export with a Deliver-style settings page
- A clean core/GUI split with tests that run headlessly and don't need a
  display

## Reading the source

- [Architecture](docs/ARCHITECTURE.md) — how `core/` and `gui/` fit
  together, the timeline model, the playback and export pipelines
- [Dependencies](docs/DEPENDENCIES.md) — exactly what we link and why,
  with per-distro package lists
- [Building & Testing](docs/BUILDING.md) — build script, install/uninstall,
  the 11-test suite
- [Current State](docs/CURRENT-STATE.md) — what works, known issues, recent
  changes

## Quick notes

- **Engine is Qt-free on purpose.** `core/` never includes Qt; a build-time
  guard (`scripts/check_qtdep.sh`) enforces it.
- **Zero-warning rule** — every build and test target must compile clean; a
  warning is treated as a defect.
- **License:** MIT, © 2026 Mattscreative.
