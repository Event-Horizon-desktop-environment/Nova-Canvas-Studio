# VAAPI — Full Research & Known-Issues Reference

> Companion to `docs/vulkan.md`. Documents the decision, current engine wiring,
> driver landscape, and — most importantly — **every known VAAPI issue + the fix**
> that can bite Nova Canvas Studio's timeline preview, media-pool thumbnails,
> source preview, and VAAPI export. Written 2026-09-12 from web research; driver
> facts dated where known. This is the "AMD route": CUDA remains the NVIDIA-only
> fast path, VAAPI is the AMD (and Intel) hardware decode/encode route.

---

## 1. TL;DR / decision record

| Question | Answer |
|---|---|
| Does the app support AMD today? | **Yes — VAAPI.** `HwDeviceManager` probes `cuda → vaapi → qsv → vulkan` and picks whatever init succeeds first; on an AMD box `radeonsi` wins and every preview decodes in hardware. |
| Was that the intent? | Originally CUDA/NVIDIA-only was requested; on reflection VAAPI was chosen as the AMD route. zluda was considered and **rejected for decode**: zluda translates the CUDA runtime/ABI for *shader compute*, but FFmpeg's `cuda` decoder uses **NVDEC**, a hardware codec block that only exists on NVIDIA silicon. You cannot get NVDEC from an AMD GPU through any translation layer — **VAAPI is the only hardware decode route on AMD** (see §11). |
| Does VAAPI give the same GPU fast path as CUDA? | **No.** The zero-copy GPU composite path (`decode_to_hw`, `decode_to_hw_indexed`, `decode_nv12`) is **CUDA-only** in the engine today. VAAPI decodes on the GPU but every frame is downloaded to CPU (NV12 → swscale → RGBA). Hardware decode still offloads the CPU heavily; it just doesn't keep the picture on-GPU like NVDEC does. See §9 "engine gaps". |
| Known issue density | Decode: low but real (driver/Mesa-version dependent artifacts, P010 handling, no error concealment). Encode: **higher** — rate-control wiring, packed/global headers, HEVC P-frame corruption on specific VCN parts, the HEVC 64×16 bar bug. See §7. |

**Bottom line:** VAAPI is correct for AMD. Treat decode as "seriously production but
watch Mesa version + color/10-bit edge cases", treat VAAPI *encode* as "works; the
quality-path wiring now maps crf/rc onto VAAPI's rc_mode/global_quality knobs
(§8.1, shipped 2026-09-12) but some AMD VCN parts have 2026 encode regressions
(§7.D)".

---

## 2. What the engine uses today (code-ground truth)

All paths below are the VAAPI surfaces for timeline preview / media preview /
source preview / export. Line numbers verified against the tree on 2026-09-12.

### 2.1 Device probe — `core/src/media/hw_device.cpp`
- `kProbeOrder[] = {"cuda", "vaapi", "qsv", "vulkan"}` (`hw_device.cpp:16`) — on
  AMD, `radeonsi` init succeeds and `device_name_ = "vaapi"`.
- `av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0)` (`hw_device.cpp:57`) —
  **null device** → FFmpeg auto-derives the VA display (Wayland → X11 → DRM) and
  auto-picks the first render node. This is the source of the multi-GPU and
  headless pitfalls in §7.A.
- Probe timing + winner are always logged (`[hw] probing accelerators in order…`,
  `[hw]   vaapi: selected in %.1f ms`) — first thing to grep when previews fall
  back to software.

### 2.2 Decode — `core/src/media/video_decoder.cpp`
- `open()` scans `avcodec_get_hw_config(cand, i)` for a decoder matching the
  device type and sets `hw_pix_fmt_` + a `get_format` callback
  (`video_decoder.cpp:401–482`). With a VAAPI device the hw pix fmt is
  `AV_PIX_FMT_VAAPI`, hw decoders are `h264_vaapi` / `hevc_vaapi` / `av1_vaapi` /
  `vp9_vaapi`.
- If the codec can't actually decode in hardware, FFmpeg emits **software frames
  from a hardware-configured decoder** — the engine detects this and re-anchors /
  latches `soft_only_` (`video_decoder.cpp:1081–1096`).
- `make_rgba_frame()` is the VAAPI download path: `av_hwframe_transfer_data(sw, src, 0)`
  pulls NV12/P010 to CPU, then a pinned swscale converts to full-range RGBA using
  the file's resolved matrix/range (`video_decoder.cpp:1225–1323`).
- **The GPU fast paths are CUDA-only by design:**
  `decode_to_hw`/`decode_to_hw_indexed` bail unless `hw_pix_fmt_ == AV_PIX_FMT_CUDA`
  (`video_decoder.cpp:965`, `:1177`). On VAAPI these never fire → §9.

### 2.3 Timeline / source preview — `gui/src/features/playback/timeline_decoder.cpp`
- Shares one `HwDeviceManager hw_{"playback"}` across every per-media decoder slot
  and the transition/re-anchor decoders (`timeline_decoder.cpp:77`, `:112`,
  `:936`, `:1059`). Timeline preview and source preview (which rides on
  `SequenceController` → `TimelineDecoder`) therefore share the same device.

### 2.4 Media preview / thumbnails — `gui/src/features/thumbnails/thumbnail_service.cpp`
- Each worker owns its own `HwDeviceManager hw{"thumbs"}` (`:288`) and decodes via
  `decoder.open(req.path, &error, hw.device_ctx())` (`:537–540`). Same download+
  swscale path as §2.2, so media-pool thumbnails are VAAPI-decoded too.

### 2.5 Export — `core/src/export/exporter.cpp` + `deliver_preset.cpp`
- `EncoderBackend::AMD → h264_vaapi / hevc_vaapi / av1_vaapi`
  (`deliver_preset.cpp:27,90–112`).
- `hw_device_for_codec("…vaapi…") → "vaapi"` (`exporter.cpp:96–103`); VAAPI is in
  `available_hw_devices()` (`:159–174`).
- Encode session: `AV_PIX_FMT_VAAPI` pixels, hw-frames pool with `sw_format = NV12`,
  `initial_pool_size = 12`, `gop_size = 120`, `max_b_frames = 0`
  (`exporter.cpp:362–364`, `:470–491`). CPU composited RGBA → swscale → NV12 →
  `av_hwframe_get_buffer` + `av_hwframe_transfer_data` upload per frame
  (`:580–589`, `:788/873/907/1161/1222/1257`).
- **Quality path is VAAPI-aware (§8.1, shipped 2026-09-12):** `crf`/`rc` intent
  is mapped onto VAAPI's `rc_mode`/`global_quality`/`qp` private options for
  `*_vaapi` codecs (`exporter.cpp:385–391` via `vaapi_encode`); non-VAAPI codecs
  keep the crf push. Bitrate-driven modes gain `rc_mode=CBR/VBR` for VAAPI
  (`exporter.cpp:414–416`).

---

## 3. The VAAPI stack

```
your app / FFmpeg / GStreamer / mpv
        │  vaGetDisplay*/vaCreateContext/vaRenderPicture…
        ▼
libva  (free, X11 Foundation; dlopens a backend .so)
        │  LIBVA_DRIVER_NAME, LIBVA_DRIVERS_PATH
        ▼
driver  radeonsi_drv_video.so  (AMD, Mesa — this app's AMD route)
        │  iHD_drv_video.so     (Intel, intel-media-driver, Broadwell+)
        │  i965_drv_video.so    (Intel, libva-intel-driver, GMA4500→Coffee Lake, legacy)
        │  nouveau_drv_video.so (NVIDIA open driver, GeForce 8→GTX 750, needs nouveau-fw)
        │  libva-nvidia-driver  (elFarto nvidia-vaapi-driver: VAAPI → NVDEC decode via VDPAU interop, no CUDA)
        ▼
hardware  AMD UVD (GCN-era) / VCN (Vega+), Intel QuickSync/GFX codec engines,
          NVIDIA NVDEC/NVENC (via the adapter or Vulkan Video)
```

**Who wins the probe in this engine:** on AMD boxes, only `radeonsi` initializes →
`vaapi` selected → decode on. If no VAAPI driver or hardware is present, the probe
logs `no accelerator selected; falling back to software decode` and every fallback
path in the engine handles it.

---

## 4. Driver matrix (who is where, updated 2026)

| Driver | Hardware | Decode | Encode | Notes |
|---|---|---|---|---|
| `radeonsi` (Mesa) | Radeon HD 2000 → RDNA4 | MPEG2/VC-1/H.264 all GCN; HEVC8 GCN3+; HEVC10 GCN4+; VP9 Raven Ridge + RX5000+; AV1 RX 6600+ | H.264 HD7000+; HEVC8 R400+; HEVC10 Raven Ridge+; AV1 RX 7900+ | **The AMD route for this app.** All codecs via UVD/VCN. |
| `iHD` (intel-media-driver) | Broadwell + (incl. Arc) | Broadwell+ full stack incl. AV1 (TGL+) | Full stack (AV1 Arc+) | Gen9/G12 iGPUs; see §7.A init bugs. |
| `i965` (libva-intel-driver) | GMA 4500 → Coffee Lake | H.264/VC1/MPEG2, VP8, HEVC (SKL+) | As supported | **Deprecated**, last release 2.4.1; no AV1; collision with `crocus`. |
| `nouveau` (Mesa) | GeForce 8 → GTX 750 | MPEG2/VC-1/H.264 | none | Needs `nouveau-fw` (fw extracted from NVIDIA binary driver); firmware bugs noted upstream. Mostly historical. |
| `libva-nvidia-driver` (elFarto) | NVIDIA Fermi+ | NVDEC via VAAPI shim | **none** | Decode-only; VA-API is not first-party NVIDIA. NVDEC via VDPAU interop, **no CUDA**. Notably higher power draw than CPU decode historically (`CUDA_DISABLE_PERF_BOOST=1` on NVIDIA ≥580.105.08); main purpose is Firefox. VA-API encode on NVIDIA exists only in unofficial forks (`nvidia-vaapi-driver-nvenc`). |
| Mesa 25.3+ | — | — | — | **Removed VDPAU from open drivers** (`radeonsi` included). Any doc/snippet telling AMD users to set `VDPAU_DRIVER=radeonsi` is obsolete; use VAAPI. |

**Distro packaging traps (§7.E):**
- **Fedora / RPM Fusion:** stock Fedora Mesa ships without patent-encumbered codecs —
  H.264/H.265/VC-1 VAAPI decode is **absent** from `radeonsi_drv_video.so` until you
  install `mesa-freeworld`. `vainfo` showing no H264/HEVC profiles on an otherwise
  fine AMD card is the signature.
- **Snapcraft-included Mesa:** Snap-bundled `<23.1.1` kills H.264/VP9 on AMD
  (Firefox bug 1859291) — distro/host Mesa is newer but the snap's is old.
- **Debian:** periodic "radeonsi: no VAAPI support (regression)" packaging bugs.

---

## 5. Hardware decode caps that matter for an editor

| Capability | Radeonsi/UVD·VCN | iHD | Notes |
|---|---|---|---|
| H.264 max level | GCN: mostly ≤ 4.1/5.1 → **1080p** reliably; 4K only on newer VCN | 5.2 | Old GCN1–2 parts fail 4K H.264 decode (`No support for codec h264 profile 100` / blocky). |
| HEVC 10-bit (P010) | GCN4+ (Polaris) | Broxton+ | Works on modern parts; presentation path is where bugs live (§7.B/e). |
| VP9 10-bit | Raven Ridge+ | Kaby Lake+ | Intel iHD had a broken-VP9-profile2 stretch (Kodi #23649 / media-driver #1172); works via raw FFmpeg. |
| AV1 | RX 6600+ | Tiger Lake+ | iHD AV1 decode needs the GmmLib-fixed build (§7.A). |
| VC-1 / MPEG-2 | All GCN | Broadwell+ | **VCN4 (RDNA3) dropped MPEG-2/VC-1 spec-level decode**; files fall back to software. |
| Max resolution | Varies 4K8K by gen (see rocDecode matrix: H.265 7680×4320 on VCN3+) | Scale-dependent | Old UVD caps H.264 at 4096×2160; 8K streams go soft. |
| Error concealment | **None exposed** | None | Hurt/truncated/corrupt sources show artifacts or stall; software decoders conceal. §7.B/c. |

Engine consequence: on modern AMD (RDNA2+, VCN3+) the realistic editor mix
(H.264/HEVC 8/10-bit up to 4K, AV1) is fully hardware-decodable. Anything that's
not → FFmpeg emits software frames from the hw-configured decoder and the engine
serves them transparently (with the `soft_only_` latch + logs).

---

## 6. Verification playbook (know it works before blaming the app)

```bash
vainfo                                   # profiles present -> driver + codecs live
ls -l /dev/dri/renderD*                  # which render nodes exist (multi-GPU!)
nvtop | amdgpu_top                       # watch VCN % while previewing
ffmpeg -hwaccel vaapi -i sample.mkv -f null -   # quick decode smoke test
```

Engine-side, the always-on logs tell you everything:
- `[hw] probing accelerators in order: cuda vaapi qsv vulkan (owner=playback)`
- `[hw]   vaapi: selected in X.X ms` (decode will be hw)
- `[decode] sws_rgba src_fmt=…` (first CPU conversion per format/size — shows the
  resolved matrix/range, includes the VAAPI download)
- `decode open: … hw=yes hw_pix=…` per file open
- grep `soft_only=1` to detect the "configured for hw but decoding on CPU" state.
- If hw decode is working, **CPU % at playback must drop** dramatically vs software.

If `[hw] no accelerator selected` on an AMD box: §7.A causes first (wrong render
node, driver missing/packaging), then §5 caps.

---

## 7. KNOWN ISSUES + FIXES (the core of this doc)

### A. Probe / init / device-selection

**A1. Wrong GPU selected on multi-GPU / hybrid boxes.**
FFmpeg's VAAPI device init with a null device auto-picks a default display and
render node. On a box with an NVIDIA + AMD pair (or iGPU + dGPU), the *wrong* node
can be opened. On Intel/iHD the signature is `DRM_IOCTL_I915_GEM_APERTURE failed:
Invalid argument` + `get chip id failed: -1` (intel/media-driver #1845). Same class:
`DRM_IOCTL_VERSION, unsupported drm device by media driver` when forcing
`LIBVA_DRIVER_NAME` at the wrong node.
*Fix:* enumerate `/dev/dri/renderD*`, probe each, pick the one whose driver name/
`vainfo` output matches the GPU you want; pass the explicit node to
`av_hwdevice_ctx_create` (`device = "/dev/dri/renderD128"`), and fall back to the
next node on init failure. iHD upstream says applications "should try each drm
node" — our `av_hwdevice_ctx_create(…, null, …)` currently doesn't.
*App status:* **gap** — single auto-picked device only.

**A2. iHD init failures (Intel).**
- Older kernels (e.g. 6.8.0-40) + iHD = `vaInitialize failed with error code 18`.
  Driver/kernel coupling — update media-driver then kernel.
- `undefined symbol: _ZN6GmmLib…` from `iHD_drv_video.so` = ABI mismatch
  (older GMM/GmmLib). Fix: `intel-media-driver-legacy` (AUR) matching the
  libva/libva-clint gens, or rely on the `i965` fallback (Arch's stock setup loads
  iHD, fails, falls back to i965 which works for Coffee Lake).
- B580/Battlemage (Xe): VAAPI init crash with some media-driver builds
  (intel/media-driver #1998). Fix: update media-driver past the crash build.
*App status:* AMD route unaffected; Intel preview boxes are the risk.

**A3. Headless / wrong-DISPLAY init.**
`libva: … init failed` when `$DISPLAY` is set but no X server, or Wayland-only
sessions mis-detected.x — Chromium/docs blame improper Wayland detection.
*Fix:* unset `DISPLAY`, or force DRM: FFmpeg's explicit `vaapi=…:/dev/dri/renderD128`
device, or run `vainfo` with the `drm` display. In headless CI/renders, always pass
the explicit render node.
*App status:* the `[hw]` probe uses null devices everywhere (`hw_device.cpp:57`,
`exporter.cpp:467`). On a headless render box the null device still lands on DRM
eventually, but explicit-node is the robust route.

**A4. Old AMD parts with missing profiles.**
Southern Islands / Oland (GCN1) sometimes fail to advertise H.264 decode
(Red Hat bug 1841991: `No support for codec h264 profile 100.`; VAAPI shows only
MPEG2). Not a bug in the app — the card/driver combination has no profile.
*Fix:* upgrade Mesa (very old profiles were rectified later), or accept the
CPU fallback. Nothing to fix in code.

**A5. Probe latency pollution.**
`cuda` probing on a machine with no NVIDIA GPU spins the CUDA runtime (~300ms
reported in this repo's own logs). Not VAAPI-specific, but it delays the VAAPI
outcome on AMD boxes and logs as a startup stall.
*Fix:* probe cheapest-first or gate by `/dev/nvidia*`/`lspci` before
`av_hwdevice_ctx_create(cuda)`.

### B. Decode

**B1. H.264/VP9 decode artifacts on AMD (Mesa < 23.1.1).**
Known artifact period on RX 6600-class parts ("AMD Radeon 6600: video corruption",
fixed in Mesa 23.1.1 — referenced by Firefox bug 1859291 and the 23.1.1 release
notes). Chromium users got green/artifacts until the host (or snap) Mesa fixed it.
*Fix:* Mesa ≥ 23.1.1. Nothing to change in app.

**B2. AMD `allow_rgb10_configs`, color corruption.**
ArchWiki documents video-decoding corruption/distortion under AMDGPU with
`allow_rgb10_configs=false` env/kernel-args fix (bugs.freedesktop.org 106490).
*Fix:* set the option false if RGB10 configs tear the picture (mostly older
Wayland/compositor cases; modern Mesa defaults are safe).
*App status:* our download path is NV12/P010→RGBA in swscale — the RGB10 compositor
path is not reached.

**B3. VAAPI has no error concealment.**
Unlike software decoders, VAAPI surfaces do not conceal corrupt/gap data; missing
SPS/PPS mid-stream stalls or glitches (GStreamer bug 796863, 'no concealment in
VAAPI'). Consequences for an editor: damaged files, container-extracted streams,
and **starts of GOPs after byte-seeks** can show blocks/short garbage.
*Fix (app):* the engine's keyframe-re-anchor behavior (`decode_to_hw` re-anchors to
the owning I-frame, latches `soft_only_` when the hw path can't engage) is exactly
the right mitigation. For VAAPI, never drain mid-GOP — always seek to the
I-frame before a forward walk (the `decode_to_hw_indexed` iframe table pattern).

**B4. 10-bit (P010) download / presentation edge cases.**
VAAPI drivers historically "lack proper image conversions"; `vaputimage` can fail
for advertised formats, which is why FFmpeg's `hwdownload`/`av_hwframe_transfer_data`
was patched to **force/fall back to NV12 first** (ffmpeg-devel Aug 2024, Zhao Zhili;
GStreamer same approach, bug 752958). Separate symptom class: HEVC/AV1 Main10
*drops frames/stutters* at presentation even though decode is fine (Chromium
523313377 — P010 handling in the compositor, affects two hosts incl. radeonsi), and
the elFarto NVIDIA VAAPI adapter had P010–context mismatches (cuMemcpy2D pitch
failures).
*Fix (app):* pin the decode sw_format — for 8-bit previews keep NV12 even for
10-bit sources by downconverting at download, or accept P010 and let *swscale*
(not vaapi) handle the conversion. Our `make_rgba_frame` already converts through
swscale, so the compositor never sees P010 — sidesteps the stutter class. For
*export* of 10-bit (main10), the encoder frames context must be created with
`sw_format = P010`, not NV12 (§8).

**B5. Colorspace/range mismatch after download.**
The perennial 601-vs-709 and limited-vs-full misrender. Two failure flavors on
VAAPI: (a) the driver's surface layout/chroma doesn't carry the stream's tags and
anything downstream assumes BT.601 (swscale's `SWS_CS_DEFAULT`), or (b) the source
is tagged but a fixed-function vaapi scaler converts colors without linearizing
(iHD #1833 — `scale_vaapi` ≠ `zscale`).
*Fix (app):* read the *frame's* `colorspace`/`color_range` after download (we do —
`sws_setColorspaceDetails` pinned from resolved tags + BT.601/709/2020 matrix
selection, `video_decoder.cpp:1294–1323`); never let the hw scaler do color math
into the preview (VAAPI is used for decode only; CPU swscale owns color).

**B6. VC-1 / MPEG-2 / old-codec gaps upstream.**
VCN4 (RDNA3) dropped VC-1/MPEG-2 decode; old MPEG-4 ASP is unsupported on most VA
backends. *Fix:* the automatic software-frame emission already handles these; no
code change, but don't "fix" a slow-playback report by assuming the GPU is at work.

**B7. First-frames-after-seek artifacts.**
MPV/Jellyfin both have VAAPI trickplay/seek artifact reports (Jellyfin #17133 on
H.264 + mjpeg; frame-scattering on newer-writes at GOP boundaries). Root cause is
serving a reference-dependent frame without priming its GOP.
*Fix (app):* always open at an I-frame for VAAPI seeks. The engine's iframe table +
`container_seek_seconds` re-anchor covers this — keep it active for the VAAPI
backend (make sure `decode_to_hw_indexed`-style logic is not CUDA-gated for the
vaapi download path; today it is, §9).

### C. GPU interop / zero-copy

**C1. No zero-copy preview on AMD.**
VAAPI surfaces *can* be presented directly (dmabuf via `vaExportSurfaceHandle`,
used by mpv/Firefox/Chromium), but that requires libva dmabuf interop + GL/Vulkan
import in the viewer. Chrome/libplacebo need `PL_HANDLE_DMA_BUF` and a GL/Vulkan
backend (mpv #17028/NixOS #331756: "VAAPI hwdec only works with OpenGL or Vulkan
backends"). VLC does not support it at all.
*App status:* the engine deliberately **does not** import VAAPI surfaces into
`ViewerGL` today — it downloads to CPU RGBA. Correct and simple; the cost is a
per-frame DMA read + swscale. If previews ever need zero-copy, it's dmabuf import
into the GL widget (Qt has no first-class helper; done via QOpenGLExtraFunctions +
EGLImage). Flag as a real feature gap but not a bug.

**C2. NV12 upload for export double-copies.**
Export composites on CPU to RGBA, swscale → NV12, `av_hwframe_transfer_data` into
`AV_PIX_FMT_VAAPI` surfaces, encode. That per-frame host→device copy is the normal
VAAPI encode path (hwupload's job) — NOT a bug. It only becomes a problem if
someone wires CPU RGBA → swscale for every frame *and* swscale is slower than the
encode budget; at editor scales (≤4K deliver) swscale keeps up. Keep `SWS_BILINEAR`,
not `SWS_FAST_BILINEAR`, for encode input (quality).

**C3. Decoder/encoder surface pools.**
VAAPI decode buffers are VASurface-backed; `initial_pool_size` too small →
`Failed to allocate surface` under burst (scrub). The engine's frame cache bounds
decode ahead. If surface pressure ever appears, raise the pool early and watch
`vram`/`amdgpu_top` usage rather than assuming CPU.

### D. Encode / export

**D1. `hevc_vaapi` corrupts P-frames on Van Gogh (gfx1033) since Mesa 26.0.**
Mesa 26.1.7 / 26.2.1 release notes carry the regression: **"radeonsi/VCN: HEVC
VAAPI encode corrupts P-frames on Van Gogh (gfx1033) since Mesa 26.0 — I-frames
and H.264 unaffected."** Van Gogh = Steam Deck / Phoenix-class APUs using gfx1033.
*Fix:* if the machine is gfx1033 and exports HEVC, either Mesa < 26.0 or export
H.264 (or CPU). Add a probe-time check: `amdgpu` + `gfx1033` + HEVC-vaapi encode
→ warn and pick h264_vaapi / libx265 fallback. **2026 live regression — the most
likely AMD export bug you will hit.**

**D2. HEVC `64×16` right-edge bar bug (AMD).**
Classic: hevc_vaapi on radeonsi produced a bar down the right edge for widths not
aligned to the 64×16 CTU/min-CU grid — the fix is a single FFmpeg patch
(`pahaze/ffmpeg-amd-vaapi-fix`). Upstream/Mesa fixed it; if a user's distro carries
an old patched-FFmpeg or old Mesa the bar returns at e.g. a 64-nonaligned width.
*Fix:* align export widths to multiples of the codec min-block (or QA the first
HEVC export frame against the letterbox test). `export_sweep` running on an AMD box
is the regression gate.

**D3. `Failed to end picture encode issue: 5 (invalid VAContextID)` (h264_vaapi).**
Old-school AMD error, still the signature for unstable h264_vaapi configs:
bitrate + B-frames + non-baseline profile combinations blow the VA context
(my.ffmpeg-user 2020: "Use the baseline profile, set a fixed bitrate (via -b:v)
and explicitly disable B-frames"). The engine already sets `max_b_frames = 0`
(`exporter.cpp:364`) — keep B-frames off for VAAPI H.264.
*Fix:* main/baseline profile, fixed bitrate (CBR), `-bf 0`, current Mesa.

**D4. "Driver does not support some wanted packed headers (wanted 0xd, found 0)" + "No global header will be written".**
VAAPI drivers disagree on SPS/PPS packing. When the driver won't emit packed
headers, FFmpeg warns a global header won't be written → stream may not mux to
some containers. The engine sets `AV_CODEC_FLAG_GLOBAL_HEADER`
(`exporter.cpp:377`) which *requests* them; radeonsi newer builds honor it. For
drivers that refuse, the file still plays in MP4/MKV (in-band SPS/PPS) — treat the
warning as informational; if a strict muxer rejects, force profile main /
`packed_headers=1` where the driver exposes it.

**D5. VAAPI encoder rate control ≠ NVENC quality knobs.**
`-crf` is **not** a VAAPI option. VAAPI routing is `rc_mode` (CQP/ICQ/QVBR/VBR/CBR)
plus `qp`/`global_quality`, `compression_level`, `maxrate`/`bufsize`.
Historically VAAPI *ignored* x264-style CRF silently, leaving default rate control.
*App status:* **fixed.** §8.1 (2026-09-12) maps crf/rc intent onto
`rc_mode`/`global_quality`/`qp` for `*_vaapi` codecs; `AVERROR_OPTION_NOT_FOUND`
from `av_opt_set*` is now a loud `log_warning`, never a silent no-op.

**D6. NVENC-only options silently no-op.**
`rc=`, `cq=` and split-encode/level stamping are set only under `is_nvenc` — fine.
But `surfaces` (`rc-lookahead`-derived) is applied for *all* hw encoders, including
VAAPI. `surfaces` exists on vaapi encoders; only `crf` genuinely does not (and it
is now diverted to the VAAPI path per D5).

**D7. B-frame depth vs VAAPI stability.**
Several VAAPI backends misbehave above `max_b_frames==2` (VCE-era) and modern VCN
is fine with 0 but constrained without lookahead. Engine pins 0 — safe, if
slightly lower compression-efficiency on AMD. No change needed; note it.

**D8. HDR → SDR, tonemapping, and fixed-function colors.**
`scale_vaapi`/`procamp_vaapi` don't linearize (iHD #1833; "beyond capabilities of
fixed-function hardware"). The engine composites in 8-bit BT.709 limited on CPU and
stamps tags (`exporter.cpp:368–371`) — correct for SDR deliver. If HDR deliver is
ever wanted, do tone-mapping in the CPU/GPU shader path, never in a vaapi filter.

### E. Environment & packaging

**E1. `LIBVA_DRIVER_NAME` / `LIBVA_DRIVERS_PATH` / `LIBVA_DISPLAY`** — override
driver file, search path, display. `LIBVA_MESSAGING_LEVEL` raises libva verbosity
for support calls.

**E2. `DRI_PRIME=1` (hybrid)** — pick the discrete GPU for VA-API on PRIME
setups. Some dGPU/older parts still hide H.264 (A4).

**E3. `CUDA_DISABLE_PERF_BOOST=1`** — required on NVIDIA VAAPI adapter (≥580) to
avoid the decode-consumes-more-power-than-CPU trap.

**E4. Firmware** — `linux-firmware-amd` (VCN microcode) missing/stale = decode
init failures or locked codecs; `linux-firmware-intel` needed on Skylake+.

**E5. RPM Fusion `mesa-freeworld`** — §4. Fedora AMD H.264/H.265/VC-1 decode gone
without it.

---

## 8. Engine-specific findings

### 8.1 Export quality-path wiring is NVENC-shaped — VAAPI fix now shipped
FIXED (2026-09-12). The exporter maps crf/rc intent onto VAAPI's
`rc_mode`/`global_quality`/`qp` knobs via a small Qt-free module,
`core/include/canvas/core/export/vaapi_encode.hpp` + `core/src/export/vaapi_encode.cpp`:

- `is_vaapi_codec(name)` — true for `*_vaapi` codecs.
- `vaapi_rate_control_from(codec, crf, vid_rc_mode, bitrate_kbps)` → `VaapiRateControl`:
  - `constqp` + crf≥0 → `rc_mode="CQP"`, `qp=crf*scale`, `global_quality=crf*scale`.
  - `vbr`/`vbr_target` + crf≥0 → `rc_mode="QVBR"` for hevc/av1,
    `"ICQ"` for h264 (h264_vaapi has no QVBR — verified, see §4/§7).
  - `auto` + crf≥0 → `rc_mode="ICQ"`, `global_quality=crf*scale`.
  - crf<0 + `cbr` → `rc_mode="CBR"`; crf<0 + `vbr*` → `rc_mode="VBR"`;
    crf<0 + `auto` → no rc_mode (driver default).
  - **scale is 1× for h264/hevc (0–51 range), 5× for av1 (0–255 range)**
    so `crf=23` lands exactly where the VAAPI quality axis expects it.
  - `apply_vaapi_rate_control(vctx, rc)` sets `rc_mode` (`av_opt_set`),
    then `qp` + `global_quality` together when crf-driven (`av_opt_set_int`),
    all into `vctx->priv_data` with `AV_OPT_SEARCH_CHILDREN`; each call's
    return is checked and a `log_warning` is emitted on `AVERROR_OPTION_NOT_FOUND`
    (D6-class: an absent option is a loud warning, never a hard failure).
  - Called **before** `avcodec_open2`, so `vctx->codec` is still NULL — the
    codec name for diagnostics comes from `avcodec_descriptor_get(vctx->codec_id)`
    instead (vctx->codec is not open yet).
- Wiring (`exporter.cpp:380–392`): `crf≥0` now branches — VAAPI codecs get
  `apply_vaapi_rate_control(...)`, everything else keeps the old
  `av_opt_set_int(priv, "crf", ...)`. Bitrate-driven mode adds
  `rc_mode=CBR/VBR` for VAAPI (`exporter.cpp:414–416`), mirroring NVENC's `rc=`.
- The pure mapping math is locked by `core/tests/vaapi_encode_test.cpp`
  (CMake `canvas_vaapi_encode_test`, ctest name `vaapi_encode`): CQP/ICQ/QVBR
  selection, the AV1 ×5 scale (crf 23 → qp/global_quality 115), the crf<0
  CBR/VBR/auto tails, and `is_vaapi_codec` classification. No libva/avcodec
  link needed — the test asserts only the mapping law.

Untouched (still NVENC-shaped, still fine on VAAPI):
- `max_b_frames=0` and the `surfaces` computation (maps well on vaapi).
- `cq=`/`rc=` stays NVENC-gated; VAAPI has equivalent knobs now set above.
- probe gate for gfx1033 + HEVC (D1) — still TODO in the exporter, see §7.

### 8.2 10-bit export (main10) needs a P010 sw_format
The VAAPI encode frames context is hardwired `sw_format = NV12`
(`exporter.cpp:475`). AMD HEVC 10-bit profiles (Main10/Raven Ridge+) consume P010;
NV12-only frames context makes a main10 VAAPI export either fail or silently encode
8-bit. If Main10 is ever offered on the VAAPI path, set
`fc->sw_format = AV_PIX_FMT_P010` and feed P010 (CPU sws from RGBA yields P010 —
swscale supports RGBA→P010).

### 8.3 The CUDA-only GPU fast path (§2.2) is a deliberate limit
- Preview on AMD = VAAPI decode + CPU download + swscale (still a big CPU win:
  decode offload only; RGBA conversion + upload to viewer are CPU/GL).
- No `decode_nv12`/`frame_gpu` GPU composite on VAAPI; NVENC-only GPU kernel path.
- Consequence spelled out: **on AMD, transitions/compositing never touch the GPU**
  beyond decode. Scaling the preview to more GPU work would mean dmabuf import (C1)
  or a Vulkan/ROCm compute route — out of scope here; VAAPI plays the decode role.

### 8.4 SILENT-FALLBACK audit
Places hardware silently becomes software are all handled + logged:
- no decoder for the stream-codec → `avcodec_find_decoder` software
  (`video_decoder.cpp:424`);
- hw-decoder configured but driver emits soft frames → `soft_only_` latch + log
  (`:1081–1096`);
- no accelerator at all → `no accelerator selected` (`hw_device.cpp:72`).
Any "slow preview but no [hw] error" report should be triaged with
§6's log greps first.

---

## 9. Roadmap-relevant: zluda, Vulkan, parity

- **zluda on AMD: rejected for decode.** zluda = CUDA *ABI/runtime* translation to
  ROCm/HIP. FFmpeg's `cuda` decoder is **NVDEC hardware**, not a compute kernel —
  no AMD GPU has NVDEC; no translation can invent it. zluda also cannot make our
  CUDA kernels (`nv12Resize`, grade LUT) run *faster* on AMD than the VAAPI decode
  + CPU swscale path already does. If AMD GPU-*compute* (compositing/grade kernels)
  is ever wanted, the route is ROCm-compiled kernels or Vulkan compute — **not**
  zluda, which is legally `CUDA_FORCE_PUSE`-forwarding a fragile proprietary shim
  and has no FFmpeg integrated decoder story.
- **Vulkan Video** is the genuinely cross-vendor alternative and is tracked in
  `docs/vulkan.md`. Mesa 26.0 unified the RadeonSI + RADV decoder (same VCN engine
  behind VA-API and Vulkan) — so VAAPI and a future RADV video path share hardware;
  VAAPI remains the lower-risk default.
- **Parity matrix vs NVDEC (CUDA) today:**
  | | NVIDIA/CUDA | AMD/VAAPI |
  |---|---|---|
  | HW decode | NVDEC (cuvid) | VCN via radeonsi |
  | GPU composite/grade kernel | `nv12Resize`/grade LUT (CUDA) | none (CPU) |
  | HW encode | NVENC (rc/cq/level stamped) | h264/hevc/av1_vaapi (rc_mode/global_quality stamped — §8.1 shipped) |
  | Zero-copy viewer | CUDA→NV12 plane handoff | CPU RGBA |
  | Regression gates | `gpu_grade`, `vram_leak`, `scrub_bench` | none today (§10) |

---

## 10. Regression-gate checklist (what to add for VAAPI)

1. `vainfo`-driven profile probe test — assert the AMD box exposes H.264/HEVC/AV1
   decode if the driver is present (portable: SKIP when no vaapi driver).
2. `export_sweep` on an AMD box (it already runs codec×container combos — add a
   `__vaapi` backend sweep that asserts non-zero output size + first/last frame
   bytes; gate D1 gfx1033 HEVC by encoder-name check).
3. `vram_leak`-style but VAAPI: decode a synthetic 2K H.264 loop through the
   `h264_vaapi` + `av_hwframe_transfer_data` path, sample `amdgpu_top`/DRM memory
   for steady-state drain (the `av_frame_ref`-leak class that CAUGHT NVDEC in
   this repo applies identically to VA surfaces).
4. Colorspace after VAAPI download: decode a BT.709 + a BT.2020 sample, assert the
   sws-resolved matrix log matches `video_decoder.cpp:1296–1300` selection.
5. Seek-mid-GOP VAAPI: frame-accurate target comparison with the software decoder
   (the I-frame re-anchor is the fix; a regression = garbage frames).

No VAAPI-targeted tests exist yet — the CUDA-only gates
(`gpu_grade`, `vram_leak`, `visual_render_test`) don't exercise the VAAPI path.

---

## 11. Appendix — READ THIS before wiring anything new

### 11.1 Why not zluda (one more time, precisely)
1. Decode: FFmpeg `cuda` hwaccel = NVDEC silicon (cuvid). zluda translates the CUDA
   *ABI* for kernels (`cudaMalloc`, launches) — it cannot synthesize an NVDEC block
   on a VCN card, and `h264_cuvid` requires NVDEC session creation that fails on
   AMD regardless of ABI shims.
2. Compute: our kernels could in principle build against ROCm instead, but that is
   a separate port (not zluda) and buys GPU compositing on AMD that VAAPI never
   promised. Not needed to satisfy "AMD preview support".
3. Legal/ops: zluda is a closed, version-fragile drop-in with NVIDIA-doc accretion;
   the Linux community direction (Mesa) is unified radeonsi+RADV (§9).

### 11.2 Env vars cheat sheet

| Var | Effect |
|---|---|
| `LIBVA_DRIVER_NAME` | force driver: `radeonsi` / `iHD` / `i965` / `nouveau` / `nvidia` |
| `LIBVA_DRIVERS_PATH` | where libva looks for `*_drv_video.so` |
| `LIBVA_DISPLAY` | force display (e.g. `drm`) for init |
| `LIBVA_MESSAGING_LEVEL` | libva verbosity for debugging |
| `DRI_PRIME` | hybrid: select discrete GPU |
| `CUDA_DISABLE_PERF_BOOST` | NVIDIA VAAPI adapter power fix (≥580.105.08) |
| `MESA_DEBUG` / `mesa_glthread` | driver-level debugging (GPU-side) |

### 11.3 FFmpeg CLI reference (for manual verification)

```bash
# decode to null, hw only
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i in.mkv -f null -

# transcode h264->h264 vaapi, explicit NV12 upload, no B-frames, CBR
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi \
  -i in.mkv -vf 'format=nv12,hwupload' -c:v h264_vaapi -rc_mode CBR -b:v 8M -bf 0 out.mp4

# VAAPI HEVC with explicit rc_mode qp (the D5-correct quality path)
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i in.mov \
  -vf 'format=nv12,hwupload' -c:v hevc_vaapi -rc_mode CQP -qp 22 -max_b_frames 0 out.mkv

# what the box can do
vainfo
```

### 11.4 Key tradeoffs (one-line summaries)

- VAAPI decode = free CPU headroom on AMD/Intel; VAAPI encode = works, but only with
  the right rc_mode configuration and updated Mesa (D1/D2/D5).
- The app's fixed `max_b_frames=0` + BGRA→NV12 CPU path is the *conservative*
  profile that dodges D3/D7.
- Mesa 25.3 removed VDPAU → if a support doc mentions VDPAU for AMD, it is stale.
- distro packaging (Fedora freeworld, snap-Mesa) is the #1 "hardware just shows up
  as unsupported" external cause.

---

## 12. Sources & dates (keep this fresh)

- ArchWiki *Hardware video acceleration* (2026-08-24) — driver tables, env vars,
  AMD `allow_rgb10_configs` fix, iHD/vainfo troubleshooting, Mesa 25.3 VDPAU removal.
- FFmpeg trac *Hardware/VAAPI* — AMD UVD/VCE/VCN codec table.
- ffmpeg-devel (2024-08-09) "Force vaapi image formats to NV12-only / Fallback to
  NV12" (Zhao Zhili) — driver image-conversion gaps, GStreamer parity (bug 752958).
- intel/media-driver #1845 (multi-GPU iHD init, wrong DRM node) — upstream's
  "try each drm node" guidance.
- intel/media-driver #1833 — `scale_vaapi` ≠ `zscale` color behavior.
- intel/media-driver #1998 — Battlemage/B580 VAAPI init crash (2025).
- Mesa 23.1.1 release notes — H.264 decode artifacts on AMD (RX 6600).
- Mesa 26.1.7 / 26.2.1 release notes (2026-08) — **HEVC VAAPI P-frame corruption on
  Van Gogh (gfx1033)**; ongoing in 26.2.
- Chromium issues 523313377 (VaapiVideoDecoder HEVC Main10 drops frames — present
  path), 1859291 (snap Mesa <23.1.1), 1610199 (meta: ffmpeg/VAAPI playback).
- FireFox/Mozilla VAAPI FFmpeg meta + dmabuf (bugzilla 1610199).
- Kodi #23649 / intel/media-driver #1172 — VP9 10-bit profile handling.
- Debian bug 1041647 (radeonsi VAAPI packaging regression); Red Hat 1841991 (GCN1
  H.264 profile missing).
- pahaze/ffmpeg-amd-vaapi-fix — the HEVC 64×16 right-bar patch.
- ffmpeg-user (2020-04) — h264_vaapi "invalid VAContextID", baseline/bf0/fixed-bitrate fix.
- Jellyfin #17133 — H.264 trickplay VAAPI failure (GOP-boundary issue class).
- rocDecode docs — VCN generation decode caps (max resolution per gfx).