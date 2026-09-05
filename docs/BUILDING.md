# Building, running, and testing

This project is deliberately built with boring, standard tooling: CMake,
Ninja, and whatever your distro ships. There's a wrapper script that does the
sensible thing, and the `justfile` for the install/release plumbing.

## Fast path

```sh
./build.sh            # Release build into ./build/ → ./build/gui/canvas
./build/gui/canvas    # launch (optionally: ./build/gui/canvas somefile.mp4)
ctest --test-dir build
```

`build.sh` options:

- `-d, --install-deps` — installs the dependency list from
  [DEPENDENCIES.md](DEPENDENCIES.md) via your package manager (elevates with
  pkexec or sudo, asks for confirmation first)
- `-c, --clean` — wipes `./build` and reconfigures from scratch
- `-t, --type TYPE` — `Release` (default), `Debug`, or `RelWithDebInfo`
- `-j, --jobs N` — parallel build jobs
- `-r, --no-build` — skip the build (useful with `-d` to just install deps)

Distro detection matters for dependency installs, not for the build itself.
Everything is detected by CMake at configure time. If Ninja is missing,
`build.sh` quietly falls back to Unix Makefiles.

## Doing it manually

If you prefer raw CMake over the wrapper:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Debug builds

Use `./build.sh -t Debug` or pass `-DCMAKE_BUILD_TYPE=Debug`. Two debugging
aids worth knowing:

- **Verbose logging** is gated behind an env var. Run with `CANVAS_DEBUG=1`
  to enable logging; it writes to stderr plus a log file (default
  `canvas_debug.log`, override with `CANVAS_LOG_FILE` for the filename).
- The **`roundtrip` test** is designed to be run under ASan for the edit-op /
  serialization half of the code (see Testing).

## The zero-warning rule

This repo treats compiler warnings as build failures. Every target — the GUI
app, the core library, and every test — builds under `-Wall -Wextra` and must
be quiet. If a change introduces a warning, it is flagged before being
handed off. `-Werror` is deliberately *not* enabled on the GUI app target, so
this is a discipline, not a mechanical gate — never ship a change that warns.

There are three places this applies: the Debug build, the Release build, and
the `build-release/` tree.

## The headless Qt-free guard

`build.sh` runs `scripts/check_qtdep.sh -q` at the end of every build. It
verifies that the so-called headless modules — everything in `core/` plus the
extracted GUI playback modules — contain no `#include <Q...>`. This seam is
what keeps the engine and the extraction tests buildable without a display.
If a stray Qt include sneaks in, the build fails with a clear message.

## Installing to the system

The default `./build` is a user-local build. To install into `/usr` there's a
dedicated release tree and a `justfile` on top:

```sh
just build-release                 # compile only, no install
just install                       # rebuild + install into /usr (needs sudo)
just install-release               # == just build-release, then sudo-install
just uninstall                     # removes canvas + legacy event-horizon installs
```

Install layout (matches the `.desktop` launcher):

- `/usr/bin/canvas`
- `/usr/share/applications/canvas.desktop`
- `/usr/share/icons/hicolor/scalable/apps/canvas.svg`
- plus a cleanup of the pre-rename `/usr/bin/event-horizon` and companions on
  `uninstall`

Note: `build-release/` is a separate CMake tree from `build/`. If it's owned
by root (built via `sudo just install` before), rebuild it as root again or
`sudo rm -rf build-release` first.

## Testing

```sh
ctest --test-dir build
```

11 tests, split into two families:

**Core engine** (`core/tests/`):

- `roundtrip` — edit operations + project (de)serialization round-trip.
  Runs a sector of the engine under ASan. **Known pre-existing failure** —
  it still SEGFAULTs in a leftover path (it was the oldest test in the repo
  and predates the current timeline model; see CURRENT-STATE.md).
- `export_sweep` — exports every valid codec × container combo into
  `/tmp/canvas_export_sweep/`. Returns 2 (SKIP) if `libx264` isn't available,
  which CTest reports as a normal skip.
- `scrub_bench` — a benchmark of the video decoder's seek/decode fast path;
  numbers are informational, not pass/fail.

**GUI headless tests** (`gui/tests/`) — pure-logic modules extracted from the
GUI so they can be tested with **no Qt linked and no display**. The CMake
function `canvas_add_headless_test` is what keeps them honest: it compiles the
exact production source into the test binary, so a stray Qt include fails the
build:

- `sync_constants_test` — shared constant definitions
- `timeline_decoder_test` — playback frame lookup/decoding
- `audio_pipeline_test` — audio pipeline state machine (incl. resync fixes)
- `sonicsync_test` — A/V sync logic
- `av_reanchor_test` — the MLT "audio rides with its frame" invariant
- `timeline_snap_test` — zoom-dependent snap quantization math
- `timeline_selection_test` — selection model + linked-mate coalescing
- `timeline_drag_test` — drag-session math, snap-aware, release decisions

## Launching and what to expect

`./build/gui/canvas` opens the dark, tool-grade UI: a Viewer dock, an Edit
page with a media pool, the Deliver page for export, and the timeline with its
pinned ruler/minimap/timecode strip, linked A/V clips, transitions, snapping,
undo, and the divider band that pans the channels (pull up to the limit, pull
down into the void).

Pass a media file as the first argument to load it straight into the media
pool, or use the app's open dialog.

## Troubleshooting checklists

**CUDA flag not showing in the build summary?** `build.sh` needs `nvcc` on
`PATH`. It probes `$CUDA_HOME`, `/usr/local/cuda`, `/opt/cuda`, `/usr/cuda`.
If your toolkit lives somewhere else, set `CUDA_HOME`.

**Warnings with the newest GCC?** The `.cu` kernel file is compiled with
`-allow-unsupported-compiler` to cope with nvcc lagging behind GCC. If CMake
fails on CUDA, that flag is the fix.

**No audio at runtime?** Means neither ALSA nor PipeWire dev packages were
present at configure time (both are optional). Video playback and export
still work.

**Stale app in menus?** After the rename to Nova Canvas Studio, an earlier
system install may leave an old "Event Horizon" menu entry. `just uninstall`
now removes both the new and the legacy artifacts.