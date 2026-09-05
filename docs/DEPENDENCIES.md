# Dependencies

Everything required to build and run Nova Canvas Studio, what it is used for,
and the exact package names per distribution. The short version: this is a
**Linux-only**, C++20 project built with **CMake + Ninja** on top of
**Qt6** and **FFmpeg**, with a handful of optional extras.

There are no third-party libraries fetched at build time — nothing is
downloaded, nothing is vendored into the build. Everything is expected from
your system, with one vendored-but-unused exception (see below).

## What we actually link against

| Dependency | Required? | What it's used for | Notes |
|---|---|---|---|
| **Qt 6** (Widgets + Svg) | Yes | The whole GUI shell, timeline, viewer, icons | Tested on 6.11; any recent 6.x works |
| **FFmpeg** (devel) | Yes | Decoding, encoding, muxing, resampling in the core engine | libavformat, libavcodec, libavutil, libswscale, libswresample |
| **nlohmann-json** | Yes | Project file serialize/deserialize (`*.ehproj`) | Header-only; included from `/usr/include/nlohmann` |
| **CMake** ≥ 3.24 | Yes | Build system | Ninja preferred, Makefile fallback |
| **C++20 compiler** | Yes | The codebase | GCC; `-Wall -Wextra` expected to be warning-clean |
| **Ninja** | Recommended | Build generator | `build.sh` falls back to Unix Makefiles |
| **pkgconf / pkg-config** | Yes | Detects FFmpeg, PipeWire, ALSA module lists | |
| **CUDA toolkit** (12 or 13) | Optional | NVENC fast path: CPU RGBA → NV12 + GPU resize kernels | Auto-detected via `nvcc`; sets `CANVAS_HAVE_CUDA` |
| **PipeWire** 0.3 (devel) | Optional | Audio output fallback | Detected via pkg-config |
| **ALSA** (devel) | Optional | Main audio output path | Detected via pkg-config; on absence, audio playback is disabled |

### Qt modules in detail

Qt's `qtbase` package bundles Widgets, Core, Gui, and importantly the
**OpenGLWidgets** module (the viewer is a `QOpenGLWidget`) — that's why the
package list only needs `qt6-base` + `qt6-svg`:

- `find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Svg)`
- X (the display server) and OpenGL runtime are needed at runtime, not build time.

MOC/RCC/UIC are handled by CMake's AUTOMOC/AUTORCC/AUTOUIC — no manual step.

### FFmpeg modules

The core engine does the media work with raw FFmpeg C APIs:

- **libavformat** — demuxing/muxing container files
- **libavcodec** — the codecs (H.264/H.265, AAC, etc.)
- **libavutil** — shared helpers, frame/timestamp math
- **libswscale** — pixel format conversion (decode → CPU RGBA)
- **libswresample** — audio conversion/resampling on decode

Enabled hardware encoders at export time are detected at runtime (NVENC,
VAAPI, QSV), so you do not need all of them installed to build.

## Optional extras in detail

**CUDA** — builds the GPU encode path (`canvas::core::gpu` kernels
`rgbaToNV12` and `nv12Resize`). Not on the PATH by default, so `build.sh`
probes `$CUDA_HOME`, `/usr/local/cuda`, `/opt/cuda`, `/usr/cuda` for `nvcc`
and exposes it to the build. The `.cu` file is compiled with
`-allow-unsupported-compiler` because nvcc lags the newest GCC. Without
CUDA the exporter gracefully falls back to CPU encoding.

**PipeWire / ALSA** — audio output. ALSA is preferred, PipeWire used as
fallback. Both are `pkg_check_modules(… QUIET)` in the GUI CMakeLists, so a
machine with neither simply has no audio while video still works.

## Package lists by distribution

`build.sh -d` installs exactly these (after asking for confirmation and
elevating via `pkexec` or `sudo`). The versioned package names track the
current test machine; drop the old ones in the table below is fine too.

### Fedora / dnf

```
cmake ninja-build gcc-c++ pkgconf
qt6-qtbase-devel qt6-qtsvg-devel
ffmpeg-devel
pipewire-devel alsa-lib-devel
nlohmann-json-devel
```

### Arch Linux / pacman

```
cmake ninja gcc pkgconf
qt6-base qt6-svg
ffmpeg
pipewire alsa-lib
nlohmann-json
```

### Debian / Ubuntu / apt

```
build-essential cmake ninja-build pkg-config
qt6-base-dev libqt6svg6-dev
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
libpipewire-0.3-dev libasound2-dev
nlohmann-json3-dev
```

### PikaOS

Same as Debian — it ships standard Debian packages.

## Version recap

- C++20 (no exceptions in the editing path)
- CMake ≥ 3.24, Ninja (recommended) or Make
- Qt 6.x — tested against 6.11.2; any current 6.x is fine
- FFmpeg — a recent build (post-FFmpeg-4 era); the code targets the modern
  avcodec API
- CUDA 12 or 13 if you want the NVENC fast path