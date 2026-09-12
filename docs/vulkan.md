# Vulkan Backend — Full Research & Design Plan

> Research doc + design plan for adding a second GPU backend to Nova Canvas Studio,
> everything the current stack does today re-done in **Vulkan** — decode, composite,
> colorspace conversion, resize, encode, and on-screen playback — including
> **Vulkan Video decode and encode**. Written 2026-09-11 from web research only
> (driver-matrix facts dated where known). OpenGL stays the primary backend; Vulkan
> is a second, selectable backend with a CPU/VAAPI/NVDEC fallback ladder.

---

## 1. TL;DR

| Question | Answer |
|---|---|
| Can we decode with Vulkan? | **Yes.** `VK_KHR_video_decode_queue` + codec extensions cover H.264, H.265, AV1, VP9 decode on all three desktop vendors (Intel ANV, AMD RADV, NVIDIA proprietary; NVK landing H.264 now). FFmpeg ships `h264_vulkan`/`hevc_vulkan`/`av1_vulkan`/`vp9_vulkan` hardware decoders. |
| Can we encode with Vulkan? | **Yes, with maturity caveats.** `VK_KHR_video_encode_queue` + `h264_vulkan`/`hevc_vulkan` (FFmpeg 7.1+) and `av1_vulkan` (FFmpeg 8.0) work on NVIDIA, RADV (since Mesa 24.1), and ANV (re-enabled Mesa 26.2; AV1 encode Mesa 26.3-devel). Driver ecosystem is younger than decode — see §6 war stories (Intel disabled-not-re-enabled saga, NVIDIA artifact bugs, a desktop-streaming project calling encode "premature" in Feb 2026). |
| Can we keep the pipeline on-GPU end to end? | **Yes — that is the point of the API.** Decode output images can feed graphics/compute directly (sampling an NV12/P010 multi-planar image needs only `VK_KHR_sampler_ycbcr_conversion`, core since Vulkan 1.1), and `VkVideoProfileListInfoKHR` explicitly supports decode-output-as-encode-input so a decode→composite→encode transcode stays zero-copy on one device. FFmpeg 8.x even ships Vulkan-compute codecs (FFv1, ProRes) as a second, hardware-optional path. |
| What about the viewer? | Qt gives two routes: `QVulkanWindow` embedded via `QWidget::createWindowContainer()`, or the modern `QRhiWidget` (Qt 6.7+, the portable equivalent of `QOpenGLWidget`, Vulkan-backed via QRhi with `beginExternal()`/`QRhiTexture::createFrom()` for raw-Vulkan interop). `QRhiWidget` is the recommended route. |
| Biggest risks | Encoder driver maturity (vendor-specific): the 🖥 Intel ANV encode train-wreck (disabled Feb 2026, re-enabled Jul 2026, AV1 Aug 2026), NVIDIA H.264 **decode** artifacts (fixed ≈end Oct 2026), `VK_ERROR_DEVICE_LOST` on H.265 seek on some Intel iGPUs, and old-GCN AMD being slower under Vulkan than VAAPI. Mitigations designed in (§12): probing, driver-version gates, CPU/VAAPI fallback ladder, and the existing `vram_leak`-style regression gates. |

**Bottom line:** Vulkan is a credible second backend *now* for decode + composite + present and a
*young-but-usable* backend for encode. Treat encode as "adopt after a per-GPU validation gate",
not "available on this machine today by default".

---

## 2. What Nova Canvas has today (the surface we mirror in Vulkan)

Current GPU surface, per `AGENTS.md`/docs (source truth — see the repo files for exact wiring):

1. **HW device probe** — `HwDeviceManager` probes `cuda → vaapi → qsv → vulkan` once, falls back to software. (It already mentions vulkan as a *device*, i.e. the FFmpeg vulkan context, but nothing uses a Vulkan graphics path.)
2. **Decode** — `VideoDecoder` (FFmpeg): HW decode shared-device, CPU RGBA via swscale, **low-res preview cap** (`kPreviewMaxDim` = 640), keyframe-seek vs sequential-forward heuristics. `TimelineDecoder` owns per-media `VideoDecoder`+`FrameCache` slots, a low-res scrub-preview LRU, and the shared `HwDeviceManager`.
3. **GPU fast path** — `frame_gpu` in `renderer.cpp`: single-clip compositing → NV12 → NVENC via CUDA kernels `rgbaToNV12` + `nv12Resize` (`core/src/gpu/cuda_convert.cu`, guarded by `CANVAS_HAVE_CUDA` / `cuda_available()`).
4. **Colorspace law** — `core/include/canvas/core/gpu/colorspace.hpp`: BT.601 `yuv_to_rgb`/`rgb_to_yuv`, single source of truth for the viewer's CPU fallback and the CUDA kernel contract.
5. **Composite** — `RenderSession::render_video_frame` / `render_audio_chunk` (top-down compositing; per-clip volume/pan/transform/blend/opacity; transitions).
6. **Viewer** — `ViewerGL` (`QOpenGLWidget`) with a transition shader, GPU fast path.
7. **Export** — `exporter.cpp`: NVENC/VAAPI/QSV hw encoders + CPU fallback, SFE + NVENC level stamping, progress/cancel; `list_video_codecs/list_containers/list_audio_codecs/available_hw_devices`.
8. **Regression tests** — `gpu_grade` (pins the fused CUDA grade kernel `nv12GradeResize` law by host-side mirror), `vram_leak` (NVDEC decode, >256 MB steady-state free-VRAM drain fails), `visual_render_test` (identity render byte-identical to legacy fast path; flip/scale/opacity change pixels), `export_sweep`, `scrub_bench`.

Vulkan equivalents are in the mapping table in §9.

---

## 3. Vulkan Video: the extension stack

### 3.1 What it is

Vulkan Video is a family of ratiili extensions (not core) that exposes the GPU's
fixed-function video engines through the Vulkan model — command buffers, queues,
synchronization, VkImage-based picture buffers, and query-result feedback. It is
the decoders/encoders' answer to NVENC/NVDEC/VAAPI/QSV with a single cross-vendor,
cross-platform API that "seamlessly combines GPU-powered rendering and compute with
video processing in a single efficient runtime" (Khronos, Dec 2022 finalization post).

Stack (built in layers, per the spec's *Video Coding* chapter):

```
VK_KHR_video_queue                       (base: sessions, DPB slots, video std headers, query pools)
├── VK_KHR_video_decode_queue            (decode ops; + sync2 / Vulkan 1.3)
│   ├── VK_KHR_video_decode_h264
│   ├── VK_KHR_video_decode_h265
│   ├── VK_KHR_video_decode_av1
│   └── VK_KHR_video_decode_vp9          (decoders on all three desktop vendors)
└── VK_KHR_video_encode_queue            (encode ops)
    ├── VK_KHR_video_encode_h264
    ├── VK_KHR_video_encode_h265
    ├── VK_KHR_video_encode_av1          (Vulkan 1.3.302, Nov 2024)
    └── advanced encode:
        ├── VK_KHR_video_encode_quantization_map  (1.3.302, Nov 2024)
        └── VK_KHR_video_encode_intra_refresh      (1.4.321, Jul 2025)
```

Vendor-neutral rules that matter for us:

- **Video queues.** Video decode/encode run on *separate queue families* reported via
  `VkQueueFamilyVideoPropertiesKHR::videoCodecOperations`
  (`VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR` … `ENCODE_AV1_BIT_KHR`). A physical
  device may expose video-only queues. The decode/encode command buffers share the
  command-pool/semaphore model, so decode→graphics→present is one timeline.
- **Sessions and DPB.** You create a `VkVideoSessionKHR` (profile, picture format,
  `maxCodedExtent`, `referencePictureFormat`, `maxDpbSlots`, `maxActiveReferencePictures`)
  and `VkVideoSessionParametersKHR` (codec sequence/picture parameters, read from the
  Khronos **video std headers** `vk_video/vulkan_video_codec_{h264,h265,av1}std*.h` —
  `av1std` + `av1std_encode`, `h264std`, `h265std`…). The decoded-picture buffer (DPB)
  state and backing store are split: `VkVideoSessionParametersKHR` holds DPB state,
  images (VkImage with `VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`) hold the store.
- **Picture formats.** Decode/encode picture images are multi-planar YUV, e.g.
  `G8_B8R8_2PLANE_420_UNORM` (NV12), `G10X6_B10X6R10X6_2PLANE_420_UNORM` (10-bit P010-ish),
  3-plane `G8_B8_R8_3PLANE_420_UNORM`, and 4:2:2 / 4:4:4 / mono per profile chroma
  flags (`VK_VIDEO_CHROMA_SUBSAMPLING_{MONOCHROME,420,422,444}_BIT_KHR` + per-component
  bit depth). The decode/encode picture `format` in the profile decides the exact
  codec operation (e.g. decode H264 main vs high 10 profile picks the depth).
- **Layouts.** Dedicated layouts must be used at the right time:
  `VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR`, `VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR`
  (+ the codec-specified *reference layouts*), encode equivalents. This is the main
  source of validation-layer VUID pain — the known-good sequence is literal in the spec.
- **Command flow.** `vkBeginVideoCodingKHR` → `vkCmdDecodeVideoKHR`/`vkCmdEncodeVideoKHR`
  → `vkEndVideoCodingKHR` inside a command buffer submitted to a video queue;
  `vkCmdControlVideoCodingKHR` for mid-recording changes.
- **Result feedback.** Query pools (`VK_QUERY_TYPE_VIDEO_DECODE_BIT_KHR` /
  `VK_QUERY_TYPE_VIDEO_ENCODE_BIT_KHR`) report `VkQueryResultStatusKHR`
  (`SUCCESS`/`NOT_READY`/`ERROR_...`) so you know whether decode/encode actually worked —
  decode errors are not fatal (recovery/fallback like our NVDEC heuristics).
- **Sampling.** Multi-planar decode output is consumable by graphics directly via
  `VK_KHR_sampler_ycbcr_conversion` (core since 1.1): bind planes, apply a YCbCr→RGB
  conversion with your own color matrix — *no NV12→RGBA pass needed for display*.

### 3.2 Timeline of releases

| Date | Event |
|---|---|
| 2021-04 | Vulkan Video *provisional* extensions released |
| 2022-12-19 | Khronos **finalizes** decode extensions for H.264/H.265 (Tony Zlatinski, NVIDIA); official sample is `vk_video_decode` (NVIDIA) |
| 2024-02-01 | Vulkan **1.3.277**: Decode AV1 extension + **Vulkan SDK now ships H.264/H.265 encode headers/support** |
| 2024-09 | FFmpeg merges **H.264/H.265 Vulkan encoders** (`h264_vulkan`/`hevc_vulkan`; FFmpeg 7.1 era) |
| 2024-11-21 | Vulkan **1.3.302**: **`VK_KHR_video_encode_av1`** + **`VK_KHR_video_encode_quantization_map`** → full decode+encode for H.264/H.265/AV1 (Ahmed Abdelkhalek, AMD) |
| 2024-12-02 | Vulkan 1.4 released (promotes sync2, etc. to core; video stays extension-gated) |
| ~2025-06 | `VK_KHR_video_decode_vp9` (Khronos announcement; RADV support via Mesa MR !35398; FFmpeg support arrived by FFmpeg 8.0) |
| 2025-07-09 | Vulkan **1.4.321**: **`VK_KHR_video_encode_intra_refresh`** (2nd advanced encode feature) |
| 2025-08 | **FFmpeg 8.0**: Vulkan VP9 hardware decode, **Vulkan AV1 encode** (`av1_vulkan`), Vulkan-compute codecs (FFv1 enc/dec, ProRes RAW dec) |
| 2026-03-16 | Khronos blog: FFmpeg Vulkan-**compute** codecs deep-dive (Lynne) |
| 2026 | **FFmpeg 8.1**: Vulkan-compute FFv1 enc/dec, ProRes enc/dec, ProRes RAW dec, DPX unpack; no more runtime GLSL (precompiled SPIR-V) |
| 2026-07-05/08 | Intel ANV re-enables H.264/H.265 **encode** on Gen12.5/Alchemist (Mesa 26.2) |
| 2026-08-12 | Intel ANV enables **AV1 encode** (`VK_KHR_video_encode_av1`) on DG2/Alchemist (Mesa 26.3-devel) |
| 2026-08-17 | FFmpeg lands H.265 Vulkan **encode perf parity** with H.264 (Phoronix: hevc 289–358 fps vs h264 288–359 fps @1080p) |

### 3.3 The two decode/encode implementation classes (important for FFmpeg)

For the rest of this doc, keep apart:

- **Vulkan Video (fixed-function)** — dedicated codec silicon behind
  `VK_KHR_video_decode/encode_*`; this is what mirrors NVDEC/VAAPI/QSV today.
- **Vulkan Compute (shaders)** — pure-compute codecs (FFv1, ProRes, DPX in FFmpeg
  8.0/8.1) running on *any* Vulkan 1.3 GPU with no video engine. Decoders reuse the
  same FFmpeg hwaccel API; encoders are separate (`ffv1_vulkan`). This class does not
  help H.264/H.265/AV1 encode, but it does mean *"fully Vulkan decode-filter-encode
  with no downloads"* is possible even without video hardware.

---

## 4. Decode in Vulkan — the mechanics

### 4.1 Decode operation data flow

Per frame, you record one or more `vkCmdDecodeVideoKHR` calls with a
`VkVideoDecodeInfoKHR`:

```c
typedef struct VkVideoDecodeInfoKHR {
    VkStructureType                      sType;
    const void*                          pNext;
    VkVideoDecodeFlagsKHR                flags;
    VkBuffer                             srcBuffer;        // encoded bitstream
    VkDeviceSize                         srcBufferOffset;
    VkDeviceSize                         srcBufferRange;   // bytes of bitstream
    VkVideoPictureResourceInfoKHR        dstPictureResource; // decode OUTPUT image
    const VkVideoReferenceSlotInfoKHR*   pSetupReferenceSlot;// reconstructed pic slot
    const VkVideoReferenceSlotInfoKHR*   pReferenceSlots;    // DPB reference pictures
} VkVideoDecodeInfoKHR;
```

- `dstPictureResource.imageViewBinding` is an image view on the decode-output image
  (created with `VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR`); `baseArrayLayer` selects a
  specific array layer; `codedExtent` is the coded (cropped) size.
- **DPB.** Reference pictures live in DPB images (`VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR`),
  indexed by slot. Decoders may write reconstructed pictures **COINCIDE** with the
  output (same slot as dst, then the dst image doubles as a reference — good for decode
  only) or to a **DISTINCT** reference slot. The spec explicitly spells out the
  allocate-many-output-pictures recipe to dodge write-after-write hazards across
  successive decodes — this is the buffering knob in our design.
- **Reordering is the app's job.** Bitstreams use B-frames; the app reorders displayed
  picture order using the codec header fields (H.264 POC/frame_num, H.265/AV1 slot
  lists). FFmpeg and mpv already do this; raw-Vulkan means re-implementing per codec.
- **Error handling:** query pool results (`VkQueryResultStatusKHR`); a failed decode is
  non-fatal (like an NVDEC hiccup), so scrub/seek needs "reset session + first-frames
  prep" rather than teardown.

### 4.2 What the decode extension gives the pipeline (the "everything on GPU" story)

From the `VK_KHR_video_queue` proposal/Vulkan docs, the integration promises:

- decode output images can be **sampled by graphics** (`VK_IMAGE_USAGE_SAMPLED_BIT`),
  can be written **directly to presentable swapchain images** where implementation caps
  allow, and can even be **encode input** to a later `VkCmdEncodeVideoKHR`.
- zero-copy across decode → filter/composite → encode with "minimal synchronization
  overhead", because everything is one synchronization/timeline model.
- sparse image bindings are allowed for picture resources.

Sampling multi-planar decode output in a shader:

```c
// at image allocation time:
VkImageViewCreateInfo  viewInfo;   // format = NV12 multi-planar, with
viewInfo.pNext = &ycbcrCreateInfo; // VkSamplerYcbcrConversionCreateInfoKHR
//                       (colorModel=GPU/YCBCR_601 for BT.601, rgbRange, etc.)
```
…then bind `VkSamplerYcbcrConversion` into the pipeline and sample
`vec3 sampleYuv(texture, uv)` — the hardware/YUV planes are coalesced into an RGB sample
using the matrix you configured. `colorspace.hpp`'s BT.601 law becomes the matrix
coefficients passed in, so playback/export/Inspector never drift (same rule as `audio_mix`).

> Note: for the CUDA path we deliberately *don't* do this — `nv12Resize`
> GPU-composites and downsizes in NV12 space to feed NVENC directly. In Vulkan we can
> do both: `G8_B8R8_2PLANE_420_UNORM` sampling for the viewer (no conversion pass) and a
> compute scale+RGB→NV12 for the encoder path (parity with `rgbaToNV12`).

---

## 5. Encode in Vulkan — the mechanics

### 5.1 Encode operation

Symmetric to decode:

```c
typedef struct VkVideoEncodeInfoKHR {
    ...
    VkBuffer                             dstBuffer;       // encoded output bitstream
    VkDeviceSize                         dstBufferOffset;
    VkDeviceSize                         dstBufferRange;
    VkVideoPictureResourceInfoKHR        srcPictureResource; // encode INPUT image
    const VkVideoReferenceSlotInfoKHR*   pSetupReferenceSlot;
    const VkVideoReferenceSlotInfoKHR*   pReferenceSlots;
    // codec-specific picture params in pNext (std encode headers)
} VkVideoEncodeInfoKHR;
```

- **Rate control:** `VkVideoEncodeRateControlInfoKHR` modes `VK_VIDEO_ENCODE_RATE_CONTROL_DISABLED`,
  `_CBR`, `_VBR` (plus per-codec `VkVideoEncodeH264RateControlInfoKHR` etc.); AV1 adds
  `MODE_DISABLED/MODE_CBR/MODE_VBR/MODE_QUALITY`-style params + CDF/reference-name
  mapping (`referenceNameSlotIndices`).
- **Quality/tuning:** `VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR`, `HIGH_QUALITY`,
  `LOW_LATENCY`, `ULTRA_LOW_LATENCY`, `LOSS_LESS`, `LOSSLESS`; per-codec capability
  structs (`VkVideoEncodeH264CapabilitiesKHR`, …) enumerate supported rate-control modes,
  bitrate ranges, and quality levels.
- **Elementary stream:** the app can request the implementation generate H.264
  SPS/PPS ("EmitPictureParameters") for a complete elementary stream — v1 can rely on
  the existing FFmpeg muxer instead.
- **Advanced features (optional, driver-dependent):**
  - *Quantization Map* (`VK_KHR_video_encode_quantization_map`): per-coding-block
    `epsilon`/`emphasis` maps on H.264/H.265/AV1 — could later power a "focus/blur"
    quality mask.
  - *Intra-refresh* (`VK_KHR_video_encode_intra_refresh`, 1.4.321): line-by-line IUV
    refresh for low-latency/live (not our export need).

### 5.2 Meaning for export

`list_video_codecs` gains `h264_vulkan`/`hevc_vulkan`/`av1_vulkan` behind a runtime
probe; `exporter.cpp` picks the Vulkan encoder when the device has the matching
`VK_VIDEO_CODEC_OPERATION_ENCODE_*_BIT_KHR` and the container/codec contract in
`deliver_settings_model` allows it. FFmpeg encodes expect NV12 (or P010) input, so the
composite output is `rgbaToNV12`-parity compute kernel → encode input image
(`VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR`). Zero-copy when that image is the same
`VkVideoProfileListInfoKHR`-shared image decode wrote (see §8).

---

## 6. FFmpeg integration (the pragmatic path)

We do not need to build a raw Vulkan Video session from scratch. FFmpeg ships a
full Vulkan hardware-acceleration seam and mature decoders/encoders.

### 6.1 `AV_HWDEVICE_TYPE_VULKAN`

- `av_hwdevice_ctx_create` gives an `AVHWDeviceContext` whose `hwctx` is an
  `AVVulkanDeviceContext` (inst, phys_dev, act_dev, `enabled_inst_extensions`/
  `enabled_dev_extensions` lists, queue-family indices for graphics/transfer/compute,
  `nb_qf` + per-queue `AVVulkanDeviceQueueFamily { idx, num, video_caps }`).
- **Default enabled device extensions** (when FFmpeg creates the device on its own):
  `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
  `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore_fd`,
  `VK_EXT_external_memory_host`. Default features: `shaderImageGatherExtended`,
  `fragmentStoresAndAtomics`, `shaderInt64`, `vertexPipelineStoresAndAtomics`.
- Instance API version must be ≥ 1.1. If queue indices aren't provided,
  `av_hwdevice_ctx_create` fills them (falls back to the graphics family).
- **Frames:** `AVHWFramesContext.pool` returns refs whose `data` is an `AVVkFrame`
  (`img[AV_NUM_DATA_POINTERS]` images, `mem[]` device memory, `tiling`,
  `size[]`, `flags`). `av_vk_frame_alloc()` allocates one; the pool is filled by FFmpeg.
  For user-allocated pools you hand back `AVVkFrame`s — the image/memory handles are
  then yours.

### 6.2 Decoders & encoders in FFmpeg

- **Decode (FFmpeg 6.x→8.x):** `h264_vulkan`, `hevc_vulkan`, `av1_vulkan`,
  `vp9_vulkan` hardware decoders. Standard pattern:
  `-hwaccel vulkan -hwaccel_output_format vulkan` (or `-init_hw_device vulkan`), plus
  filters. Known FFmpeg 8.0 release-line items confirm "Vulkan decode hwaccel, supporting
  H264, HEVC and AV1" and "(Vulkan VP9)".
- **Encode (FFmpeg 7.1+):** `h264_vulkan`, `hevc_vulkan` (merged Sep 2024), `av1_vulkan`
  (FFmpeg 8.0). Typical pipeline for a CPU-source encode:
  `ffmpeg -init_hw_device vulkan -i in -vf "format=nv12,hwupload" -c:v h264_vulkan -y out.mp4`.
  Options (`ffmpeg -h encoder=h264_vulkan`) include `idr_interval`, `quality`, `rc_mode`
  (rate control modes), etc. Default fallback is *fixed QP = 18* when no rate control is
  given ("No rate control settings specified, using fixed QP = 18").
- **Vulkan filters (compile→run on our frames):** `scale_vulkan`, `yadif_vulkan`,
  `bwdif_vulkan`, `gblur_vulkan`, `avgblur_vulkan`, `blur_vulkan`, `nlmeans_vulkan`,
  `xfade_vulkan`, `color_vulkan`, `overlay_vulkan` family — GPU-side up/downscale at
  preview cap, deinterlace, and the grain of most DSLR/interlaced sources.
- **Vulkan-compute codecs (8.0/8.1):** FFv1 enc/dec, ProRes enc/dec, ProRes RAW dec,
  DPX unpack — pure-shader, work on any Vulkan 1.3, decoders via the same hwaccel API.
  Not for H.264/265/AV1, but they make "no CPU video ever" reachable for archival formats.

### 6.3 Shared-device strategy (key decision)

**Recommended: create our own `VkInstance`/`VkDevice`, then hand it to FFmpeg** through
an `AVVulkanDeviceContext` we fill in (queues incl. video, extensions incl. external
memory, `nb_qf`, per-queue `video_caps`). Consequences:

- FFmpeg decode/encode produces `AVVkFrame`s on *our* device → zero copying, zero
  interop margin, one timeline semaphore set across decode/filter/present/encode.
- We own validation layers, queue selection, swapchain, memory allocator, and the
  Vulkan-Hpp RAII lifetime rules; FFmpeg owns demux/parse/bitstream-bookkeeping.
- `av_hwframe_ctx_create_derived()` is available for cross-device mapping if we ever
  need to bridge devices.
- Deriving the memory allocator (`VMA`) for `VkDeviceMemory` on exported `AVVkFrame.mem`
  is trivial since the handles ARE `VkDeviceMemory` we recognize.

Fallback ladder stays: probe Vulkan-video-capable device → else existing VAAPI/NVDEC/QSV
→ else CPU. Keep the current `HwDeviceManager` ordering (cuda→vaapi→qsv→vulkan) but let
`vulkan` rise when the device reports the needed video queue operations.

---

## 7. Driver ecosystem matrix (as of September 2026)

Synthesis of Igalia's Vulkan Video status page (last update 2026-05-22), Khronos/Phoronix
news, the mpv Vulkan-Video FAQ, and vendor forums. Treat as a point-in-time snapshot.

### Decode

| Vendor / driver | H.264 | H.265 | AV1 | VP9 | Notes |
|---|---|---|---|---|---|
| **Intel ANV** (Mesa) | ✔ (Mesa 23.2+) | ✔ | ✔ | ✔ | Seek/skip on H.265 reported `VK_ERROR_DEVICE_LOST` on some iGPUs (i3-1215U, Jul 2025; Ice Lake-era Feb 2026); H.265 pegged 3D engine 100% on some builds. |
| **AMD RADV** (Mesa) | ✔ | ✔ | ✔ | ✔ (MR !35398) | Default-on for RDNA3/VCN4+ since Mesa 24.1 (RDNA 24.1 patch). **Older GCN needs `RADV_EXPERIMENTAL=video_decode`** (replaces `RADV_PERFTEST`, Mesa MR !40646); Polaris/Vega/Fiji hit-or-miss (Polaris →5× slower than VAAPI, Jan 2026 report; artifacts fixed ~2025). Stoney GCN-3 crashes (`UVD` deprecated). |
| **NVIDIA proprietary** | ✔ | ✔ | ✔ (>550.54.14) | ✔ (≥580.xx) | **H.264 decode artifact bug on Linux** (all H.264, see war stories) — fix ≈end Oct 2026. 50-series device-lost issues until 580.76.05 (Aug 2025). Linux only via proprietary driver; no VAAPI. |
| **NVIDIA NVK** (open) | ✔ (H.264 only) | in progress (GSP wedge) | — | — | Linux 7.3 Nouveau patch + Mesa; merged for **Mesa 26.3**, Turing+ only. |
| AMDVLK | ✖ | ✖ | ✖ | ✖ | Not supported. |
| Intel Windows / AMD Windows | under-tested | ✔ (RDNA1–3) | — | — | AMD Windows decode from 22.11.2 specials → 23.9.3+ stable. |

### Encode

| Vendor / driver | H.264 | H.265 | AV1 | Status / caveats |
|---|---|---|---|---|
| **NVIDIA proprietary** | ✔ (Win 538.09 / Linux 535.43.22+) | ✔ | ✔ (Win 553.40+) | Most mature. Blackwell **AV1 encode corruption** report May 2026 (unresolved, single report). |
| **AMD RADV** | ✔ (Mesa 24.1.0) | ✔ (Mesa 24.1.0) | ✔ (Mesa 25.2) | Default-on since Mesa 24.1 ships with `video_encode` too; oldest wide driver support after NVIDIA. |
| **Intel ANV** | ✔ (Mesa 24.3) | ✔ (Mesa 24.3) | ✔ (Mesa 26.3-devel, DG2) | **Roller-coaster:** disabled Feb 2026 for Gen12.5+ (untested), re-enabled Jul 2026 (Mesa 26.2, Arc A750-tested, H.265 10-bit added Jul 2026), AV1 encode Aug 2026 (Mesa 26.3-devel; 10-bit/loop-filter/CDEF left TODO). |
| AMDVLK | ✖ | ✖ | ✖ | — |

### Encode perf reality (Phoronix, 2026-08-17)

`hevc_vulkan` after FFmpeg 2026-08 tuning ("ALLOW_ENCODE_PARAMETER_OPTIMIZATIONS",
min CU 16×16) reaches **parity with `h264_vulkan`**: 1080p testsrc2 @ quality 1, 1000
frames → hevc 289/325/358 fps vs h264 288/317/359 fps. (Also: this FFmpeg commit fixed a
confusion where `log2_diff`-only SPS copies produced undecodable HEVC after some
implementations rewrote min CU — i.e. bitstream-correctness bugs were still being fixed
in 2026.)

### War stories worth locking into the risk register

1. **NVIDIA H.264 decode artifacts** (forums.developer.nvidia.com #378828, Jul–Aug 2026):
   *all* H.264 content artifacts on RTX 5080 Laptop, driver 610.43.03; H.265/AV1 fine;
   CUDA/NVDEC fine; re-encode + remux still artifact → driver decoder bug. NVIDIA replied
   "fix available GA release scheduled around end of Oct. 2026".
2. **Intel encode disable→re-enable** (Phoronix/Khronos, Feb vs Jul 2026): disabled
   Gen12.5+ "insufficient testing" after an FFmpeg H.264 encode bug report; re-enabled
   via Igalia (Hyunjun Ko) on Mesa 26.2; then **AV1 encode** for DG2 on Mesa 26.3-devel.
3. **FFmpeg Intel encode failure** (ffmpeg-user, Dec 2024): `h264_vulkan`/`hevc_vulkan`
   on FFmpeg 7.1 → "Encode failed: **-729850096**", "Error submitting video frame to the
   encoder" — the exact failure class that later motivated Intel's disable. Output tag
   shows color oddity `vulkan(tv, bt709, top coded first (swapped))`.
4. **cosmic-ext-rdp-server** (2026-02-12): "Direct Vulkan Video encoding is premature.
   The driver ecosystem is too immature (Intel disabled, AMD still fixing reference
   bugs)" — a production desktop-streaming project explicitly declined Vulkan-Video
   encode. Good ammo for our "gate encode per-GPU" stance.
5. **H.265 seek device-lost on Intel iGPU** (mpv #13909 comments) and **H.264 device-lost
   on Ice Lake/Mesa 25.1.9** — per-codec, per-gen decode stability is uneven.
6. **Firefox** enabling Vulkan Video decode for NVIDIA on Linux (FF 153, 2026-07-21,
   opt-in `media.hardware-video-decoding-vulkan.enabled=true`); Phoronix: "Firefox has
   finally begun supporting Vulkan Video decoding… not enabled by default." Ecosystem
   adoption is real but still opt-in in places.
7. **NVK** Vulkan Video H.264 decode merged for Mesa 26.3 (Phoronix, 2026-07-31/08):
   open-source NVIDIA decode path, Turing+.
8. **Perf divergence on old hw:** Polaris RX 570 under Vulkan ≈5× *slower* than VAAPI
   (mpv thread, Jan 2026) — Vulkan Video is not automatically the fastest path on old
   silicon; probe and compare.

---

## 8. Presentation, frame pacing, and the Qt viewer

### 8.1 Frame pacing for a video editor

Playback A/V sync matters here (the sequencedetector of SonicSync). Vulkan's pacing:
- `VK_KHR_present_wait` (`vkWaitForPresentKHR`, + `VK_KHR_present_id`): the host waits
  until a present is *visible to the user*. Good enough to cap the video queue vs the
  audio position, but docs admit "loose requirements… inconsistent implementations" and
  it exposes no latency/display-property info.
- `VK_EXT_present_timing` (ratified, Dec 2025 feature; registered #209, rev 3): per-present
  **timing statistics** + **explicit target present time** (`VkPresentTimingInfoEXT`:
  `targetTime`, `timeDomainId`), plus display info (`refreshDuration`, FRR vs VRR; a
  fixed refresh's buy-a-domain should be an integer multiple of `refreshDuration`).
  Depends on `VK_KHR_swapchain` + `VK_KHR_present_id2` + `VK_KHR_get_surface_capabilities2`
  + `VK_KHR_calibrated_timestamps`. The official `swapchain_present_timing` sample shows
  closed-loop pacing from past-present statistics.
- Strategy: default to FIFO swapchain + `present_wait` to decouple from the audio clock
  (direct mapping on top of SonicSync); adopt `present_timing` when the device exposes it
  for tight display-lock. This is a *during-playback nicety*, not a build-blocker.

### 8.2 Qt widget integration

Two real options:

1. **`QVulkanWindow`** (Qt GUI, since 6.x): owns Vulkan instance/device/graphics queue/
   swapchain (double-buffered FIFO), `QVulkanWindowRenderer` subclass + `createRenderer()`
   factory; embedded into a QWidget UI with `QWidget::createWindowContainer()` (official
   *Hello Vulkan Widget* example), which has documented limitations (children-overlay
   issues, no direct compositor interop).
2. **`QRhiWidget`** (Qt Widgets, **since 6.7**, production-ish in 6.8): "the portable
   equivalent of QOpenGLWidget that is not tied to a single 3D graphics API" — Vulkan,
   D3D11/12, Metal, OpenGL. Subclass + `initialize()`/`render()`; shaders as Vulkan-style
   GLSL compiled to SPIR-V by `qt_add_shaders()`/`qsb`. Direct-Vulkan escapes:
   `QRhiCommandBuffer::beginExternal()` records native Vulkan calls inside a QRhi render
   pass, and `QRhiTexture::createFrom()` wraps an existing native `VkImage` — exactly what
   we need to composite a decode-output image into the widget's color texture.
   Caveat: QRhi/QShader classes carry limited compatibility guarantees; two widgets
   requesting different APIs in one window hierarchy → only one works.

**Recommendation:** `QRhiWidget` (Vulkan) for the new `ViewerVk`. It keeps the widget
composition we have, gives a native-Vulkan escape hatch, and is the least invasive swap
from `ViewerGL`. Keep `ViewerGL` as the default GL path; make the renderer selectable.

---

## 9. Target architecture

### 9.1 Mapping "what we have today → Vulkan"

| Today (any backend) | Vulkan equivalent |
|---|---|
| `HwDeviceManager` cuda→vaapi→qsv→vulkan | keep; add a real Vulkan branch in the probe order; select physical device by video-capable queue |
| NVDEC/VAAPI/QSV HW decode | `VK_KHR_video_decode_{h264,h265,av1,vp9}` via `h264_vulkan` etc. on our device; VC1/MPEG2/VP8 left on old APIs |
| swscale → CPU RGBA (viewer strip) | sample the multi-planar image with `sampler_ycbcr_conversion` (BT.601 matrix from `colorspace.hpp`) — no RGBA pass |
| `kPreviewMaxDim`(640) low-res strip | encode a small `codedExtent` in the session, or `scale_vulkan` pass |
| `rgbaToNV12`/`nv12Resize` CUDA | executability compute passes w/ identical laws (`colorspace.hpp`, `visual.hpp`), pinned by a `gpu_grade`-style host mirror |
| `RenderSession` compositing | graphics/compute pass: top-down SrcOver (`co = Cs·αs + Cb·αb·(1−αs)` from video.md), transform/blend/opacity/rotation laws |
| transition shader (ViewerGL) | same Cartesian/linear math in SPIR-V (qsb-compiled) |
| `FrameCache` LRU (CPU) | GPU frame pool (decode-output ring) + keep a small sysmem mirror for export parity when needed |
| NVENC/VAAPI/QSV export | `h264_vulkan`/`hevc_vulkan`/`av1_vulkan` on the same device; `list_video_codecs` gated by probe |
| SFE + NVENC level stamping | SPS/PPS emit or the FFmpeg muxer path |
| viewer `ViewerGL` | `ViewerVk` (`QRhiWidget`) |
| `gpu_grade` / `vram_leak` / `visual_render` | Vulkan variants (§11) |

### 9.2 New headless module: `core/src/gpu/vulkan/`

Qt-free, matches the headless invariant (`gui/src/features/playback/`, `Widgets/` rules —
this bank of code goes under `core/`, tests via `canvas_add_headless_test`):

- `vk_context.hpp/.cpp` — instance/device creation with Vulkan-Hpp RAII
  (`<vulkan/vulkan_raii.hpp>`); queue-family discovery reporting `videoCodecOperations`;
  enable `VK_KHR_video_queue` + codec decode/encode + `VK_KHR_external_memory_fd`,
  `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
  `VK_KHR_external_semaphore_fd`; sync primitives (timeline semaphores); debug/pref
  validation layer wiring (`VK_LAYER_KHRONOS_validation`, debug-only). Mirror of
  `cuda_available()` → `vk_video_available()`.
- `vk_frame_pool.hpp/.cpp` — decode-output ring (multiple images to dissolve WAW), DPB
  slot bookkeeping, AVVkFrame<->our-pool glue (for the FFmpeg hybrid path).
- `vk_video.hpp/.cpp` — thin RAII over `VkVideoSessionKHR`/`VkVideoSessionParametersKHR`,
  per-codec profile structs, both decode and encode paths, query-result feedback.
- `vk_composite.hpp/.cpp` — the graphics/compute passes: multi-planar sample, scale
  (nv12Resize parity), RGB→NV12 (rgbaToNV12 parity), transform/blend/opacity/rotation,
  SrcOver, preview-cap downscale. All shaders SPIR-V via glslang/shaderc at build time
  (or `qsb` when compiled in the GUI) so there's no runtime compilation.
- `vk_present.hpp/.cpp` — optional; swapchain glue + pacing hooks (present_wait /
  present_timing) for the GUI. GUI-side thin adapter owns the `QRhiWidget`.

Separation: `core/gpu/vulkan/*` is pure Vulkan + FFmpeg (libavutil) with **no Qt**; the
Qt-visible viewer is a `QRhiWidget` shell that receives a ready `VkImage`.

### 9.3 Queue/sync design

- One physical device, one logical device; queue families: graphics, transfer, compute
  (whichever exist), plus video-decode and video-encode families when present.
- Cross-family work ordered by **timeline semaphores** (core 1.2): decode queue →
  graphics/composite → present/encode. Layout transitions are recorded in the same
  command buffers (video-decode layouts around `vkCmdDecodeVideoKHR`).
- Present queue: the `QRhiWidget`/swapchain presents FIFO; `present_wait` bounds in-flight
  frames so the audible position stays the master (SonicSync linkage unchanged).

### 9.4 Zero-copy decode→composite→encode (the flagship)

```
media file ─► demux/parse (FFmpeg) ─► VkBuffer (bitstream)
    ▼  VK_KHR_video_decode_*  (video-decode queue)
decode output image  G8_B8R8_2PLANE_420_UNORM  (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR
                                                + VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
                                                + VK_IMAGE_USAGE_SAMPLED_BIT)
    ├─► graphics/compute: sample + scale + transform + blend ─► present (viewer path)
    └─► compute RGB→NV12 layout fix-ups if needed ─► VK_KHR_video_encode_* (encode queue)
        └─► VkBuffer (bitstream) ─► mux
```

Mechanics, all taken from the Vulkan docs/proposals:

- `VkVideoProfileListInfoKHR` at image creation carries **at most one decode profile and
  any number of encode profiles**; the image created with those usage bits is compatible
  with all listed profiles. `VkVideoDecodeUsageInfoKHR::videoUsageHints` also honors
  `VK_VIDEO_DECODE_USAGE_TRANSCODING_BIT_KHR` (decode-output-as-encode-input cases).
- The `VK_KHR_video_queue` text: "the output of a video decode operation can be used as
  an input to a video encode operation… decode operations may directly output to
  presentable swapchain images, or to images that can subsequently be sampled by graphics
  operations… zero-copy with minimal synchronization overhead."
- External memory/semaphores fill any boundary the API itself can't (window system,
  cross-process, future NVENC-vs-Vulkan mixtures): dma-buf fd import/export, which is
  exactly what Chromium's own Vulkan-Video integration plan uses ("DMA-BUF FD export for
  zero-copy output").
- Respect the WAW buffers: at least 2 (spec talk) to many decode output pictures queued
  because the decode queue keeps writing; picture order reordering needs a display-order
  FIFO on the app side for codecs with B-frames.

### 9.5 The two bake-able paths vs the CUDA fast path

We keep today's CUDA NV12 path fully; the Vulkan backend is strictly additional.

---

## 10. Migration plan

Phases keep each step independently testable and revertible, in the style of the
`splitplan.md` phases. **Nothing is behaviour-changed until P-B.**

- **P-A — Runtime & probe.** `core/src/gpu/vulkan/vk_context.*`; `vk_video_available()`,
  physical-device/queue-family report, headless selftest (a `vulkaninfo`-like dump of
  queue families + video codec operations for the record). Validation layer wiring in
  Debug. No app behaviour change.
- **P-B — Viewer swap.** `ViewerVk` (`QRhiWidget`) alongside `ViewerGL`; presents the
  existing CPU RGBA frames (a GL-less escort of our current viewer). Byte-identical
  pixels; transition shader ported to SPIR-V; `visual_render_test`-style parity.
- **P-C — Decode on GPU.** `TimelineDecoder` gains a Vulkan decode path (FFmpeg
  `h264/hevc/av1_vulkan` on our shared device). Low-res scrub preview from Vulkan images.
  CPU + old HW paths remain. New `vram_leak`-style gate (§11).
- **P-D — Composite in Vulkan.** Multi-planar sampling + scale + transform/blend/opacity
  + SrcOver in compute/graphics; `frame_gpu` mirror. Parity tests vs CPU ladder.
- **P-E — Export on Vulkan.** `av1_vulkan`/`hevc_vulkan`/`h264_vulkan` registered when
  probed; RGB→NV12 kernel parity with `rgbaToNV12`; `export_sweep` extended; correctness
  gate (encode → decode → frames + ffprobe → PASS).
- **P-F — Zero-copy end-to-end.** Multi-profile images (`VkVideoProfileListInfoKHR`,
  TRANSCODING hint), decode→composite→encode on one device, seek with clean DPB reset,
  present on swapchain where caps allow. `vram_leak`-equivalent steady state.
- **P-G — Hardening.** `-Werror` manual zero-warning discipline on all Vulkan targets,
  `check_qtdep.sh` coverage for the new headless modules, per-driver test matrix gates,
  docs update (`AGENTS.md`, `ARCHITECTURE.md`), optional `CANVAS_VULKAN` default-on or env/CLI
  toggle to make rollout safe.

---

## 11. Test & parity strategy

Recast every existing GPU gate into a Vulkan mirror (and keep both):

- **`gpu_grade_vulkan`** — pins the fused Vulkan composite kernel's law (the
  `nv12GradeResize` equivalent) by host-side mirror: identity / limited-range / CPU-export
  parity byte-identical, plus flip/scale/opacity "does change pixels" checks.
- **`vram_leak_vulkan`** — synthesise 2K H.264, Vulkan-decoded at native res, composite to
  present, host-download samples, `vkGetPhysicalDeviceMemoryProperties`/
  `VK_EXT_memory_budget` steady-state VRAM drain **< 256 MB** fail (mirrors the NVDEC gate,
  which pinned the `av_frame_ref`-onto-dirty-`retain_hw_` leak).
- **`visual_render_parity`** — identity render byte-identical across CPU, GL, Vulkan.
- **Encoder sweep** — every valid `deliver_settings_model` (container × codec) combos incl.
  `av1_vulkan`/`hevc_vulkan`/`h264_vulkan`, verified by ffprobe + decode-roundtrip.
- **Scrub bench** — p95 scrub-preview latency under the Vulkan path, compare vs NVDEC.
- **Driver-matrix gating** (CI): catalog = detected driver + Mesa version +
  `videoCodecOperations`; encode tests skip when the driver < its known-good floor
  (the Mesa/NVIDIA floors in §7). This is what makes "not available on this machine
  today by default" concrete.

---

## 12. Risks & mitigations

| Risk | Evidence | Mitigation |
|---|---|---|
| **Encode immaturity / driver churn** | Intel disable→re-enable→AV1 (2026); cosmic-rdp calling it "premature" (Feb 2026); Blackwell AV1 corruption report | Probe + driver-floor gates; keep NVENC/VAAPI as default encode; `h264_vulkan` only when `VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR` + known-good driver; explicit user-facing "experimental" for av1_vulkan |
| **Decode artifact/crash reports** | NVIDIA H.264 artifacts (fix ≈Oct 2026); Intel H.265/H.264 seek `DEVICE_LOST`; Polaris slow + artifact history | Per-driver version hints in the probe; seek-with-session-reset path; CPU fallback ladder; opt-in `hwdec=vulkan` spirit (NVDEC/VAAPI remain default until parity proven) |
| **API/tooling churn** | extension revisions still bumping (decode_queue rev 8); header churn; video std headers evolve | Track Vulkan-Headers via git, vulkan_raii.hpp generated bindings; pin tooling in CI |
| **Zero-copy layout correctness** | video layouts + sharing hazards are the top VUID source | Validation layers in Debug; golden layout sequence from spec/samples; `gpu_grade`-mirror tests |
| **Present/timing availability** | `VK_EXT_present_timing` not everywhere; `present_wait` loose | FIFO + audio-master by default; escalate to `present_timing` only when present |
| **Old-GPU perf regressions** | Polaris 5× slower under Vulkan than VAAPI | Per-device capability probe chooses the best available backend; fallback ladder |
| **Two-widget API conflict** (QRhi limiting) | QT docs: only one API per window hierarchy works | Single RHI backend per top-level window; document mixed-GPU/Ui edge cases |
| **Display/latency edge on Wayland** | waylandvk complaints in mpv thread | Keep GL default until Vulkan present path is proven on the compositor in use |

Everything encoding-adjacent is explicitly **iterated behind a probe**, staged behind
phase gates, and every phase has a keep-the-CPU-parity test.

---

## 13. Open questions / spikes (run before P-E/P-F commit)

1. **Hybrid vs raw decode session** — measure `h264_vulkan` FFmpeg-hybrid decode
   throughput + seek latency + artifact freedom vs a raw `VkVideoSession` on our two
   target GPUs (an AMD VCN4-class + an NVIDIA). FFmpeg-hybrid is the expected winner for
   effort/robustness; raw wins only if scrub/scratch session tuning demands it.
2. **`VK_EXT_memory_budget`** availability on current Mesa/NVIDIA for the leak gate.
3. **P010 (10-bit) path** — confirm per-driver 10-bit decode format lists; if sources are
   HDR, decide viewer tonemap scope (out of v1; note it).
4. **Present: X11 vs Wayland** — swapchain format/modifier interop with the compositor
   for composed video vs the simpler widget-blit route; prefer blit in v1.
5. **Intel AV1 encode 10-bit / loop-filter gaps** — whether to advertise AV1 encode at
   all on ANV until those drop (they were TODO at Mesa 26.3-devel).
6. **`RADV_EXPERIMENTAL` inheritance** — confirm the exact Mesa release line where
   `video_decode`/`video_encode` moved from `RADV_PERFTEST` to `RADV_EXPERIMENTAL` and
   which gens still need it (Polaris/Vega/Fiji per the mpv thread).

---

## 14. References

**Khronos / Vulkan docs**
- Vulkan spec *Video Coding* chapter: docs.vulkan.org/spec/latest/chapters/videocoding.html
- `VK_KHR_video_queue`, `VK_KHR_video_decode_queue`, `VK_KHR_video_encode_av1`
  proposals + spec PDFs (github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/)
- `VkVideoDecodeInfoKHR`, `VkVideoEncodeInfoKHR`, `VkVideoPictureResourceInfoKHR` refpages
  (docs.vulkan.org/refpages/latest)
- Khronos blog: *Khronos Finalizes Vulkan Video Extensions…* (2022-12-19);
  *Encode AV1 & Encode Quantization Map* (2024-11-21, Vulkan 1.3.302);
  *Vulkan Video Encode Intra-refresh* (2025-07-09, 1.4.321);
  *VP9 Decode* (~2025-06); *AV1 Decode in Vulkan Video + SDK H.264/H.265 encode* (2024-02-01);
  *Video Encoding and Decoding with Vulkan Compute Shaders in FFmpeg* (Lynne, 2026-03-16);
  *VK_EXT_present_timing: the Journey to State-of-the-Art Frame Pacing* (2025-12-04)
- `VK_KHR_present_wait`, `VK_EXT_present_timing` refpages; `swapchain_present_timing` sample
- Igalia *Vulkan Video status* page (blogs.igalia.com/vjaquez/vulkan-video-status, 2026-05-22)
- Vulkan-Hpp repo + `vulkan_raii.hpp` + RAII programming guide; Vulkan tutorial (RAII, validation layers)
- Vulkan Validation Layers overview (VK_LAYER_KHRONOS_validation, VUIDs)

**FFmpeg**
- libavutil/hwcontext_vulkan.h `.c` (default enabled ext list; `AVVulkanDeviceContext`,
  `AVVulkanDeviceQueueFamily.video_caps`, `AVVkFrame`)
- vulkan.org news: *H.264/H.265 Vulkan Encoder Support Merged Into FFmpeg* (Sep 2024);
  *Intel ANV H.264/H.265 encode* (Mesa 24.3)
- FFmpeg 8.0 announcement (pipermail ffmpeg-devel 2025-August/347886); FFmpeg homepage release notes (Vulkan HW* items)
- ffmpeg-user Dec 2024: *h264_vulkan/hevc_vulkan encode fails on Intel, FFmpeg 7.1*
- Phoronix: *FFmpeg Lands H.265 Vulkan Encode Performance Optimizations* (2026-08-17)

**Mesa / drivers / ecosystem**
- OpenMandriva `enable-vulkan-video-decode.patch` (RADV_PERFTEST flip, Mesa 24.1.0-rc1);
  Phoronix *RADV Enables Vulkan Video By Default For RDNA3 / VCN4*
- Phoronix: *Intel Driver Disabling Vulkan Video Encode* (Feb 2026); *…Encode Now Working
  For Intel Alchemist GPUs* (Jul 2026, Mesa 26.2); *Intel ANV Driver Enables Vulkan Video
  AV1 Encoding For DG2/Alchemist* (Aug 2026, Mesa 26.3-devel); *NVK Vulkan Video Merged
  For Mesa 26.3* (+ Nouveau/Linux 7.3 NVDEC channel patch)
- Linux Journal: *Intel's Linux Vulkan Driver Adds AV1 Video Encoding* (2026-08-11)
- cordless-rdp: cosmic-ext-rdp-server issue #24 (encode "premature", 2026-02-12)

**Playback ecosystem**
- mpv discussion #13909 *Vulkan Video Decoding: Usage Guide and FAQ* (requirements, matrix,
  env vars `ANV_DEBUG=video-decode`/`ANV_VIDEO_DECODE=1`, `RADV_PERFTEST=video_decode` →
  `RADV_EXPERIMENTAL`, codec coverage, war stories incl. Polaris perf and Intel seek
  DEVICE_LOST, VP9 extension tracking)
- mpv issue #9675 (Vulkan Video hwdec via FFmpeg — "gpu surface directly")
- NVIDIA forums #378828 (H.264 decode artifacts, ~Oct 2026 fix); #378933 (H.265 device
  removal); Phoronix on Firefox 153 Vulkan Video decode (opt-in)
- NVDEC v13 API/app-note docs (decode baseline for perf comparisons)
- poniesandlight.co.uk — *Vulkan Video Decode: First Frames* (engine decode + present
  experience: 14× 1080p50 @ 60 fps)
- Khronos Chromium *Vulkan Video Decode Integration Development Plan* (DMA-BUF FD export
  zero-copy; DRM), Mozilla bug 1753129 (Vulkan Video in Firefox history)
- KhronosGroup/Vulkan-Video-Samples (`vk_video_decode`, `vk_video_encode`)

**Qt**
- Qt 6.11: `QVulkanWindow`, `QRhiWidget` (Widgets), `QRhi` (GuiPrivate), `beginExternal`,
  `QRhiTexture::createFrom`, `qt_add_shaders`/`qsb`; *Hello Vulkan Widget Example*,
  *Simple/Cube RHI Widget Example*

---

# PART II — Deep-dive findings (10 research rounds, 2026-09)

This half is the raw-research record that sections 1–14 distil, plus everything learned
afterwards. Rounds R1–R10 were source-driven: FFmpeg/mpv code read directly, Vulkan
refpages, conformance/CTS docs, Vulkanised-2025 talks, vendor news, and hands-on checks
of the installed toolchain (`ffmpeg -version/-encoders/-filters` on this machine).
Every claim below carries its citation in §20. Where a round *corrects* or *sharpens*
something in Part I, it says so.

## 15. How the rounds were run

| # | Question | Primary sources |
|---|---|---|
| R1 | Decode bitstream mechanics | `libavcodec/vulkan_decode.c` (1433 ln), `vulkan_h264.c` (602), `vulkan_av1.c` (671), `h2645_parse` |
| R2 | Queue families & sharing | mpv `hwdec_vulkan.c`, `VkQueueFamilyVideoPropertiesKHR` |
| R3 | NV12 / ycbcr sampling & chroma siting | FFmpeg `vulkan_decode.c` + SDL chroma-siting map, `VkChromaLocation` |
| R4 | Session memory & multi-session | `VkVideoSessionCreateInfoKHR`, `vkGetVideoSessionMemoryRequirementsKHR`, FFmpeg sess mgmt |
| R5 | Encoder rate-control reality | `vulkan_encode.c` (1173), `vulkan_encode_h264.c` (1687), `vulkan_encode.h`, NVIDIA docs |
| R6 | Corrupt-stream robustness | `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR`, `VkQueryResultStatusKHR`, inline queries |
| R7 | Present timing & swapchain mode | `VkSwapchainPresentModeInfoKHR`, `VK_KHR_present_id`, `QRhiSwapChain` |
| R8 | Conformance & CI | dEQP CTS readme/mustpass/fetch scripts, VVL source tree, Vulkanised-2025 T23/T31 |
| R9 | FFmpeg plumbing internals | `hwcontext_vulkan.h` (master), installed FFmpeg n9.0.1 `-decoders/-encoders/-filters` |
| R10 | Adoption risk & perf | Mesa 25.2/26.1/26.3 news, NVK status, IEEE encoder-quality study |

## 16. Round-by-round findings

### R1 — Decode bitstream mechanics (what FFmpeg actually does)

- `ff_vk_decode_init` is the one init for all codecs; sessions are created with
  `VK_VIDEO_SESSION_CREATE_INLINE_SESSION_PARAMETERS_BIT_KHR`, and the
  `dedicated_dpb || AV1` heuristic selects per-reference image layouts instead of a
  shared DPB; the decode op itself is a single `vulkan_decode_decode` call per frame.
- FFmpeg **always** normalizes to Annex-B at decode depth: `ff_h2645_extract_rbsp` yields
  NALUs that are re-emitted with `add_startcode` (both 3- and 4-byte forms), and SPS/PPS
  are fully CPU-parsed into StdVideo props before the frame is pushed. So the spec's
  "CPU side knows everything" is literally true in FFmpeg's implementation — the hybrid
  boundary is real and stable.
- H.264: 16-slot DPB maps `frames[]` → `VkVideoDecodeH264PictureInfoKHR<16>`; every
  active ref carries its own picture info; the engine is told refs through
  `pStdReferenceInfo` arrays.
- AV1: `vulkan_av1.c` CPU-parses into `StdVideoAV1SequenceHeader` + per-frame
  `StdVideoAV1*`; refs resolved purely through `ref_frame_idx`/`OrderHint` (no stream
  SPS/PPS redundancy after init); primary keys synthesized by the driver.
- **Consequence for Nova:** the "CPU parse + GPU decode" hybrid is what every production
  consumer (mpv, FFmpeg, GStreamer) ships today. We do NOT need a raw `VkVideoSession`
  orchestrator for parity — restoring `make_black_frame`/hold semantics on a failed query
  (R6) is the only piece the API doesn't hand us.

### R2 — Queue families & the shared device (mpv pattern)

- mpv's `hwdec_vulkan` does *not* create a second device: it reuses the one Vulkan
  context it already owns, enumerates families with
  `vkGetPhysicalDeviceQueueFamilyProperties2` + `VkQueueFamilyVideoPropertiesKHR
  .videoCodecOperations`, and registers every family that reports a video op into
  `AVVulkanDeviceContext.qf[]` as `{queueFamilyIndex, count, video_caps}`.
- `VK_KHR_internally_synchronized_queues` is the hair-growth path: mpv creates a video
  decode queue that is internally synchronized; FFmpeg then needs no external lock
  around `vkQueueSubmit` on it (`AVVulkanDeviceContext` has no per-queue mutex concept
  once this flag is on).
- Decode sessions carry `VK_VIDEO_DECODE_USAGE_STREAMING_BIT`; transfer ops on video
  queues still require the family to also report `VK_QUEUE_TRANSFER_BIT` (spec: video
  queue family must support transfer).
- Concurrency = `queueCount` sessions; reuse one session per media slot, drop/recreate
  on seek boundaries the way our `VideoDecoder` already drops on discontinuity.
- **Consequence for Nova:** our locked shared-device strategy (one `VkDevice` incl.
  video families, handed to FFmpeg) is exactly mpv's proven shape. Copy `hwdec_vulkan.c`
  as the reference for `AVVulkanDeviceContext.qf[]` population.

### R3 — NV12 / ycbcr sampling & chroma siting (parity-critical)

- NV12 *is* `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` (core desktop Vulkan 1.1, no extension);
  10-bit is `VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM` (P010). No
  `VK_EXT_ycbcr_8bit_4_2_0` needed on desktop (that ext is for the constraints without
  global ycbcr support; **dropped from this doc as unverifiable** — 404 across all
  sources).
- ycbcr sampling modes (`VkSamplerYcbcrModelConversion`/`VkSamplerYcbcrRange`) are
  **no replacement for `colorspace.hpp`'s BT.601 law**: fixed-function sampling converts
  to RGB with the *format's* recovery matrix — we need debug/parity tests to prove that
  a fixed-function `RANGE_FULL`/`RANGE_REDUCTION` pathway equals `yuv_to_rgb`, and the
  byte-exact route in §7.2/P-D is an **own shader**, not the fixed-function path.
- Chroma siting is per-codec, not per-format: AVC/MPEG-2 = half-horizontal
  (MPEG-2-style; H.264/AVC in FFmpeg/SDL is "half-H" = cosited vertical + left-ish),
  JPEG = centered, H.265/H.266 + AV1 → sample position declared in the bitstream or
  driver default MIDPOINT. FFmpeg does **not** set `VkSamplerYcbcrConversionCreateInfo
  .chromaOffset` from the bitstream today — it keys off the *subsampling* of the AV
  pixel format (`ff_vk_subsampling_from_av_desc`) and leaves siting default.
- **Consequence for Nova:** keep 4:2:0 (NV12) from decode through composite to encode;
  do RGB conversion ourselves (parity with `colorspace.hpp`), and treat any
  fixed-function ycbcr path as *preview-only*. Filmstrip/thumbnail decode should stay on
  the existing CPU path (or a Vulkan RGBA path) so thumbnail colors never drift from
  viewer colors.

### R4 — Video session memory & multi-session (what the session actually owns)

- `VkVideoSessionCreateInfoKHR`: `queueFamilyIndex`; `VK_VIDEO_SESSION_CREATE_INLINE_SESSION_PARAMETERS_BIT_KHR` needs **maintenance2**; `VK_VIDEO_SESSION_CREATE_INLINE_QUERIES_BIT_KHR` needs **maintenance1**; `pictureFormat`/`referencePictureFormat` are format *lists* (choose the superset); `maxCodedExtent` must sit within `[minCodedExtentSize, maxCodedExtentSize]`; `maxDpbSlots`/`maxActiveReferencePictures` capped by device props.
- `vkGetVideoSessionMemoryRequirementsKHR` gives driver-bound layout **for the session's internal context only**. The DPB and the coded pictures are plain images (external-memory capable) that *we* supply as resource handles. So one session per stream costs only driver-context memory — the VBV for a dozen streams is a few images each, not sessions.
- FFmpeg: `dedicated_dpb` sessions allocate one image per reference; layered DPBs reuse one multi-layer image; AV1 always `dedicated_dpb`. mpv's `vk_decode` opens sessions per boundary/stream and reuses across consecutive frames — our `TimelineDecoder` per-media slot is the same granularity.
- **Consequence for Nova:** the "session per stream, images shared across sessions/devices" mental model is correct; VRAM accounting = decode formats + composite, session context ≈ negligible.

### R5 — Encoder rate-control (what the API promises vs. what drivers do)

- Modes: `DEFAULT`/`DISABLED`/`CBR`/`VBR`. `DISABLED` → constant QP (H.264/H.265
  `constantQp`, AV1 `constantQIndex`). CBR/VBR need per-layer
  `VkVideoEncodeRateControlLayerInfoKHR` (≤8 layers: maxBitrate, avgBitrate, …) chained
  onto `VkVideoEncodeRateControlInfoKHR`. RC *algorithms* are **not standardized** — only
  the mode semantics are; exact bitrate is driver-defined (± similar to NVENC ~5–10%).
- `qualityLevel` (CRF-analog) is set at session creation via
  `VkVideoEncodeQualityLevelInfoKHR`, bounded by
  `GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR.maxQualityLevels`; FFmpeg clamps
  with a warning. NVIDIA maps **P1..P7** NVENC presets → quality levels 1..7 in its SDK
  docs (`p1` fastest through `p7`; p4 ≈ medium). NVIDIA CBR **ignores minQp/maxQp**
  (documented); its VBR is the "VBR with QP caps" note. Use `cqp` for internal proxy
  encodes, `cbr`/`vbr` for deliverables — and test actual bitrate tolerance vs. the
  deliverable panel's "don't blow past the target size" expectation.
- FFmpeg `vulkan_encode.h` option surface: `qp` (-1..255, default -1), `quality`
  (0..INT_MAX, default 0), `rc_mode auto|driver|cqp|cbr|vbr`, `tune default|hq|ll|ull`,
  `usage`; `-h` for `h264_vulkan` adds `profile constrained_baseline|main|high|high444p`,
  `level 1..6.2`, `coder cabac`; defaults among safety: `bf=2`, `g=300`, receive-packet
  API (`FF_CODEC_RECEIVE_PACKET_CB`).
- **Consequence for Nova:** encode gate stays per-driver. `cqp` + `quality` is the
  internal/editor path (deterministic-ish, fastest to bet); deliverable exports should
  still default to NVENC/VAAPI/software until VK encode parity on the machine's driver is
  demonstrated by the P-E encode→decode→ffprobe round-trip.

### R6 — Corrupt-stream robustness (the one gap the API leaves open)

- Per-frame error detection = `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` + `VkQueryResultStatusKHR`
  (`NOT_READY`/`SUCCESS`/`ERROR`), gated by `queryResultStatusSupport`. With
  `INLINE_QUERIES` the result comes back off the submit (`VkVideoBeginCodingInfoKHR
  .pInlineQueryResults`) with implicit sync — no separate query pool, simplest for our
  single-submit decode path.
- On a failed decode the output picture is **undefined**; using a DPB slot whose source
  failed is also legal-but-undefined. The spec has **no concealment** — drivers emit
  garbage rather than hold. mpv/FFmpeg today just surface the frame.
- **Consequence for Nova:** we own error concealment — on ERROR result, hold the last
  good frame (immediately seekable, matches `VideoDecoder`'s keyframe-seek + sequential
  heuristics) or push `make_black_frame()`. `vram_leak_vulkan`/`visual_render_parity`
  must exercise a mid-stream corrupt NAL (synthesize a corrupt packet, expect held frame,
  no crash, VRAM steady).

### R7 — Present timing & swapchain mode (frame pacing deep-dive)

- Modes: **FIFO** (the only *required* mode; vsync-locked queue; may block in
  `vkQueuePresentKHR` when full) · **MAILBOX** (single-entry, replaces pending, no
  tearing, latest-ready) · **IMMEDIATE** (tears) · **FIFO_RELAXED** (FIFO that skips to
  latest when late). `VkSwapchainPresentModeInfoKHR` (via `VK_KHR_swapchain_maintenance1`,
  alias of `VK_EXT_swapchain_maintenance1`) changes the mode **per-present** without
  recreating; images already queued under the old mode are unaffected.
- Present-id/wait: `VK_KHR_present_id` (Kay, tracked as Vulkan Docs register #295, May
  2019; Keith Packard/Valve) + `vkWaitForPresentKHR` — RADV and ANV support it,
  proprietary NVIDIA too; useful to cap video render-ahead vs. audio and to know when a
  frame actually hit glass, but docs call implementations "loose/inconsistent".
- **QRhi (Qt 6.7+/6.8+):** `QRhiSwapChain::PresentMode` = `Default` (FIFO) or `NoVSync`
  (maps to `VK_PRESENT_MODE_IMMEDIATE_KHR`) — **no MAILBOX exposure**. Flags:
  `UsedAsTransferSource`, `MinimalBufferCount` (forces 2 buffers; Vulkan backend defaults
  to 3). `QRhiWidget::update()` is throttled to the presentation rate by design, and
  `vkQueuePresentKHR` under FIFO may block → present from the GUI thread (our decode
  worker is decoupled by frame slots + SonicSync, so this is free).
- **Decision (locks §8.2):** timeline viewer = **FIFO + `present_wait`**, audio-master for
  frame pacing; escalate to `present_timing` (`VK_EXT_present_timing`, Dec-2025 feature)
  only where exposed. MAILBOX is unobtainable through QRhi — accept the limitation;
  present-mode control stays QRhi's, matching the "swapchain stays QRhi's" architecture.

### R8 — Conformance & CI

- CTS coverage: `dEQP-VK.video.decode.*` and `dEQP-VK.video.encode.*`; sample clips
  fetched by `external/fetch_video_decode_samples.py` / `…_encode_samples.py`; observability
  by dump flags `--deqp-vk-video-decode-dump=disable|single|separate` and
  `--deqp-vk-video-encode-dump=disable|yuv|bitstream|all`. Mustpass =
  `external/vulkancts/mustpass/main/vk-default.txt`. Encoders pass when output PSNR > 30 dB,
  decoders when decode + compare-sample parity holds.
- **Lavapipe (software) encode exists** (Vienna University thesis, Vulkanised-2025 T23:
  H.264 100% pass, PSNR > 30 dB) — but software **decode is not upstreamed**, so a
  no-HW CI runner can validate encode only. RADV VCN5 (RDNA4) on Mesa 25.2: "Passes CTS,
  all H.265 encode tests skipped due to minimum width requirement"; h265-encode is width-gated.
- VVL: unified `VK_LAYER_KHRONOS_validation`; video checks live in
  `layers/core_checks/cc_video.cpp`, best-practices in `layers/best_practices/bp_video.cpp`,
  state in `layers/state_tracker/video_session_state.{cpp,h}`; syncval is the
  submit-time hazard validator. **There is no `docs/video.md` in VVL** (404 verified via
  GitHub API) — the doc list has no dedicated video page.
- `VkConformanceVersion` is queried via driver props (mesa: `VK_DRIVER_INFO`/`vkGetPhysicalDeviceFeatures2`), the standard NPOT for driver-quality gating.
- **Consequence for Nova:** CI matrix = { caval, mustpass subset, decode+encode dump
  flags } when a video-capable GPU is present; otherwise skip decode, run encode-only
  against Lavapipe for plumbing smoke (not quality).

### R9 — FFmpeg plumbing internals (current source + installed toolchain)

- `hwcontext_vulkan.h` (master): `AVVulkanDeviceQueueFamily { idx, num, flags,
  video_caps }` in `qf[64]` — **the legacy named queue-family fields are gone**; video
  registration is via `video_caps`. Device must be instance **≥ 1.3** (FFmpeg requires
  1.3 for some helpers), `VK_KHR_internally_synchronized_queues`, timeline semaphores on
  `AVVkFrame` (`sem[]`/`sem_value[]` = per-format timeline values; FFmpeg waits on the
  video submission's timeline for ordering); `AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE` marks
  CUDA-interopability. `av_vkfmt_from_pixfmt` drives the NV12/P010 choices.
- **Hands-on, this machine (FFmpeg n9.0.1, libavutil 61.1.101, gcc 16):** `-decoders`
  lists **no Vulkan decoders at all**; `-encoders` lists `av1_vulkan`, `ffv1_vulkan`,
  `h264_vulkan`, `hevc_vulkan`, `prores_ks_vulkan`; filters include `scale_vulkan`,
  `transpose_vulkan`, `overlay_vulkan`, `xfade_vulkan`, `color_vulkan`, `flip_vulkan`,
  `gblur_vulkan`, `nlmeans_vulkan`, `v360_vulkan`, `bwdif_vulkan`, … — **there is no
  `colorspace_vulkan` and no `vpp_vulkan` in `libavfilter` master** (both 404 on the raw
  GitHub mirror; `git.ffmpeg.org` is Anubis-gated, so the GitHub mirror is the canonical
  reader). `vulkan_encode.c` uses the quality-level path: probe
  `GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR`, clamp
  `quality > maxQualityLevels` w/ warning, `session_create.referencePictureFormat =
  pictureFormat`, then `VkVideoEncodeRateControlInfoKHR` + per-codec structs.
- **Consequence for Nova (`vulkan.md` §10 P-C):** distro FFmpeg cannot decode
  h264/hevc/av1_vulkan today. Options: (a) self-build FFmpeg with
  `--enable-hwaccels --enable-decoder=h264_vulkan,hevc_vulkan,av1_vulkan` as a build/CI
  provision documented in BUILDING.md; (b) fallback to decode-via-NVENC-adjacent paths.
  This is a **pre-req / change to §6** — the whole Vulkan-decode story rides on an FFmpeg
  build that ships the Vulkan decoders.

### R10 — Adoption risk & perf (the 2026 ecosystem sweep)

- **AMD — the year's biggest maturity leap.** Mesa 26.1-devel (2026-02-12): **RadeonSI and
  RADV now share one unified video-decode implementation** (David Rosca's ~6k-line
  refactor) covering VCN, VCN JPEG, and **UVD** engines — RADV Vulkan Video works on legacy
  Hawaii (R9 290) too. VA-API and Vulkan Video now run the same silicon decoder code; the
  "two divergent paths" era (RadeonSI Vaapi vs RADV Vk) is over. RADV encode H.264/H.265
  (Mesa 24.1 default) + **AV1 encode merged Mesa 25.2** (Airlie/AMD/Radu). Perf reality:
  hevc_vulkan after FFmpeg's 2026-08 tuning = h264_vulkan parity
  (289/325/358 vs 288/317/359 fps @1080p q1).
- **Intel — per-gen landmines, encode back on track.** ANV decode H264/H265/AV1/VP9 since
  23.2+, but three per-gen artefacts in flight: AV1 decode corruption on Gen12 (TGL/DG1/
  RKL/ADL) from a warmup-omission **fixed in Mesa 26.1-devel** (Wa_1508208842,
  Vulkan-Video-Samples issue #187: ADL GT2 broke on 26.0.2, fixed 26.1); H.265 seek
  `DEVICE_LOST` on Ice Lake / i3-1215U; H.264 `DEVICE_LOST` on Ice Lake/Mesa 25.1.9. Encode
  was **disabled Feb 2026** ("insufficient testing", Phoronix), **re-enabled Jul 2026 in
  Mesa 26.2** (Arc A750-tested; H.265 10-bit added), **AV1 encode on DG2 lands Mesa
  26.3-devel** (10-bit/loop-filter/CDEF still TODO). A750/Alchemist is the most stable
  public Intel encode platform today.
- **NVIDIA — proprietary mature, NVK (open) arriving.** Proprietary driver = most mature
  Vulkan encode+decode (H264/H265/AV1/VP9) — but carries the **H.264 decode artifact bug on
  Linux** (all H.264; fix ≈ Oct 2026) and the 50-series device-lost saga (fixed 580.76.05).
  NVK (open): conformant Vulkan 1.3 (Turing/Ampere/Ada) then **1.4 for all supported
  GPUs** (Kepler→Ada + consumer Blackwell); **Vulkan Video decode merged for Mesa 26.3**
  (H.264 first, Turing+), gated additionally by a **Nouveau patch for Linux 7.3** opening
  the NVDEC channel (`NVK_EXPERIMENTAL=video`); encode has *no* NVK timeline yet.
- **Encoder quality reality check (applies to all HW encoders, VK Video included):** IEEE
  (paper 10937033) BD-rate study on UHD: GPU encoders (NVENC) need **>20% more bitrate
  than libx264 / librav1e** for equal PSNR/VMAF on H.264 and AV1; only HEVC had libx265
  slightly ahead (7.5% less bitrate). So VK encode ≈ "hardware encoder quality" — good for
  proxies/previews, but deliverable presets must not silently promise x264-class CRF.
- **Distro-toolchain truth:** the installed n9.0.1 ships Vulkan **encoders**, no Vulkan
  **decoders** (R9) — pairing this with RADV's unified-decode momentum means `hevc_vulkan`
  on modern AMD is close, but production users are one distro-FFmpeg rebuild away.
- **Adoption verdict fit:** decode+composite+present is credible now (optionally reachable
  via an FFmpeg rebuild); encode stays per-GPU gated; the whole path must be curated by
  driver-floor gates shipping in P-A/P-E.

## 17. Per-vendor deep dive (AMD / Intel / NVIDIA)

### AMD (RADV — default Vulkan driver on Linux)

| Axis | Status (Sept 2026) |
|---|---|
| Decode | H.264/H.265/AV1/VP9 default-on for **VCN4+ (RDNA3)** since Mesa 24.1; older GCN via `RADV_EXPERIMENTAL=video_decode`. **Unified RadeonSI+RADV decoder since 26.1** — same engine behind VA-API and Vulkan, legacy UVD/Hawaii included. CTS passes on VCN5 (RDNA4); older gens: Polaris report ≈5× slower than VAAPI (mpv) — probe preferred backend per device. |
| Encode | H.264/H.265 default-on since 24.1; **AV1 encode merged 25.2**; per-frame bitrate tolerance driver-defined (expect ~5–10%, like NVENC). H.265 min-width CTS skip on VCN5 → wide-GOP exception for narrow proxy resolutions. |
| Conformance/CI | Passes CTS decode+encode (H.265 encode width-gated); `videoCodecOperations` + `queryResultStatusSupport` probe direct. |
| Nova gates | Mesa ≥ 26.1 for decode (unified path); ≥ 25.2 for AV1 encode; ≥ 24.1 for H264/H265 encode. RADV is the reference driver for P-D/P-F parity. |

### Intel (ANV — iGPU + Arc)

| Axis | Status (Sept 2026) |
|---|---|
| Decode | H.264/H.265/AV1/VP9 since 23.2+; **per-gen carve-outs**: AV1 Gen12 warmup bug → fixed 26.1-devel (Wa_1508208842); H.265 seek `DEVICE_LOST` on some iGPUs as late as Feb 2026; H.264 `DEVICE_LOST` on Ice Lake/Mesa 25.1.9. Arc discrete is the most stable ANV video surface. |
| Encode | H.264/H.265 unreliably in 24.3, **disabled Feb 2026**, **re-enabled Jul 2026 (Mesa 26.2)** for Gen12.5+ (Arc A750-verified; H.265 10-bit Jul 2026), **AV1 encode DG2 26.3-devel** (10-bit/loop-filter/CDEF TODO). |
| Conformance/CI | CTS-reported pass on Arc for decode+encode (Vulkanised-2025/Igalia status); VVL + syncval track ANV. |
| Nova gates | Decode probe checks Mesa ≥ 26.1 for AV1 on Gen12; encode gated ≥ 26.2 (and Arc, not iGPU-first). ANV is the beta-est vendor at encode; keep NVDEC/VAAPI/QSV fallbacks. |

### NVIDIA (proprietary + NVK)

| Axis | Status (Sept 2026) |
|---|---|
| Decode (proprietary) | H.264/H.265/AV1/VP9 — most mature in Linux VK; **H.264 decode artifact bug on Linux** (fix ≈ Oct 2026); 50-series device-lost until 580.76.05; no VAAPI equivalent, this IS the NVIDIA Linux decode path. |
| Decode (NVK, open) | **Vulkan Video decode merged for Mesa 26.3**, H.264 first, Turing+; needs **Nouveau/Linux 7.3 NVDEC-channel patch**; `NVK_EXPERIMENTAL=video`. 1.4-conformant on supported GPUs (Kepler→Ada/Blackwell). |
| Encode | Proprietary: H.264/H.265 (Win 538.09 / Linux 535.43.22+), AV1 (553.40+); NVENC P1–P7 ↔ VK quality levels 1–7. **No NVK encode timeline.** Blackwell AV1 encode corruption (single report, May 2026) — watch. |
| Nova gates | NVIDIA users run the proprietary driver for VK decode/encode; treat NVK as experimental until 26.3 + Linux 7.3 ship as a stable combo. CUDA path remains the NVIDIA default in Nova (no one equals NVENC today). |

## 18. Deltas the deep-dives force on the plan (§10)

- **P-A:** instance must be ≥ 1.3 for the FFmpeg shared-device rationale (R9); populate
  `AVVulkanDeviceContext.qf[].video_caps` for every family reporting an op (R2); probe
  `queryResultStatusSupport` + `maxQualityLevels` + `VkConformanceVersion` in the
  `vulkaninfo`-style dump (R6/R8); wire synval + video sections of `VK_LAYER_KHRONOS_validation` in Debug (R8).
- **P-B:** unchanged (viewer from CPU RGBA).
- **P-C:** *new pre-req* — the distro FFmpeg build has **no Vulkan decoders** (R9): ship a
  self-built FFmpeg (or a build-script provision) with `--enable-decoder=h264_vulkan,
  hevc_vulkan,av1_vulkan`; document in BUILDING.md. FFmpeg-hybrid decode (R1) + one
  session per media slot + DPB reset on seek (R4); error-concealment hook on
  `VkQueryResultStatusKHR` (R6).
- **P-D:** own RGB conversion shader for parity (not fixed-function ycbcr) (R3); chroma
  siting per codec handled at sample-position choice; keep NV12 through composite (R3).
- **P-E:** option mapping (§16 R5): `rc_mode=cqp`+`quality` for internal, `cbr`/`vbr` for
  deliverable; driver-floor gates AMD ≥ 26.1 (decode) / ≥ 25.2 (AV1 encode) / Intel ≥ 26.2
  (encode) / NVIDIA proprietary serial-tracked artifact fix (R10); encode→decode→ffprobe
  round-trip stays the correctness gate; add PSNR/BD-rate sanity in release notes only.
- **P-F:** zero-copy multi-profile images + TRANSCODING hint; inline-query results
  (`VkVideoBeginCodingInfoKHR.pInlineQueryResults`) for per-frame success (R6); present
  = FIFO + `present_wait` everywhere (R7).
- **P-G:** optional deep CI gate runs `dEQP-VK.video.decode.*`/`encode.*` (mustpass subset)
  when a video-capable GPU is present (R8); keep Lavapipe encode-only smoke (R8);
  drop-in CLI `-hwaccel vulkan` should *not* be the default until a driver-floor check
  passes (R10).

## 19. New risk-register rows (append to §12)

| Risk | Evidence | Mitigation |
|---|---|---|
| **Distro FFmpeg lacks VK decoders** | n9.0.1 `-decoders` (this machine, Sep 2026) | Self-build/bundle FFmpeg w/ Vulkan decoders; document; block P-C otherwise |
| **libavfilter has no colorspace_vulkan / vpp_vulkan** | master `libavfilter` tree (raw GitHub, R9) | Own 4:2:0→RGBA shader in P-D, not an ffmpeg filter chain |
| **ANV encode disabled-then-re-enabled** | Phoronix Feb/Jul 2026 (R10) | Encode floor = Mesa ≥ 26.2 + Arc-first; recheck per release |
| **Per-gen decode landmines (Gen12 AV1, Ice Lake seek, Polaris)** | Wa_1508208842 (26.1-devel), mpv issues (R10) | Driver-floor + device-id keyed probe; seek resets session; opt-in VK decode |
| **Hardware encoders need >20% more bitrate** | IEEE 10937033 BD-rate (NVENC) | Deliverable presets use measured bitrate, not CRF parity; publish expectation |
| **H.265 encode CTS min-width skip** | Mesa 25.2 VCN5 CTS note (R8) | Don't advertise VK H.265 encode for < specified widths; probe supports |
| **NVIDIA H.264 decode artifact saga** | forums #378828 (R10) | NVIDIA proprietary gated by driver build (fix ≈ Oct 2026); NVK not yet a decode default |

## 20. References (Part II)

**FFmpeg source (master, read via GitHub mirror — `git.ffmpeg.org` is Anubis-blocked)**
- `raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/vulkan_decode.c`
- `…/libavcodec/vulkan_h264.c`, `…/libavcodec/vulkan_av1.c`
- `…/libavcodec/vulkan_encode.c`, `…/libavcodec/vulkan_encode_h264.c`, `…/libavcodec/vulkan_encode.h`
- `…/libavutil/hwcontext_vulkan.h` (`qf[].video_caps`, instance ≥ 1.3, `AVVkFrame.sem[]`)
- Installed toolchain: `ffmpeg -version` (n9.0.1, libavutil 61.1.101), `-decoders`, `-encoders`, `-filters`
- vulkan.org news: *H.264/H.265 Vulkan Encoder Support Merged Into FFmpeg* (Sep 2024)

**mpv**
- `video/out/hwdec/hwdec_vulkan.c` (shared-device qf[] registration, session-per-stream)
- mpv issue thread #13909 (usage+FAQ, Polarisd perf, per-gen instability)

**Vulkan spec / refpages (docs.vulkan.org/refpages/latest/refpages/source/)**
- `VkVideoSessionCreateInfoKHR.html`, `vkGetVideoSessionMemoryRequirementsKHR.html`,
  `VkVideoDecodeInfoKHR.html`, `VkVideoEncodeInfoKHR.html`, `VkVideoEncodeRateControlInfoKHR.html`,
  `VkVideoEncodeRateControlModeFlagBitsKHR.html`,
  `VkVideoEncodeQualityLevelInfoKHR.html`, `VkVideoBeginCodingInfoKHR.html`,
  `VkSwapchainPresentModeInfoKHR.html`, `VkChromaLocation.html`
- `VK_KHR_present_id` (register #295, May 2019, Keith Packard/Valve) — Vulkan-Docs proposals/registry
- Vulkan `docs.vulkan.org/spec/latest/chapters/videocoding.html`

**Conformance / CI**
- `chromium.googlesource.com/external/deqp/+/HEAD/external/vulkancts/README.md` (+ `mustpass/main/vk-default.txt`)
- `fuchsia.googlesource.com/third_party/vulkan-cts/…` (fetch_video_decode/encode_samples scripts, dump flags)
- `github.com/KhronosGroup/Vulkan-ValidationLayers` — `layers/core_checks/cc_video.cpp`,
  `layers/best_practices/bp_video.cpp`, `layers/state_tracker/video_session_state.{cpp,h}`;
  `docs/video.md` 404-verified (no video page)
- Vulkanised-2025 T23 (Vienna/Lavapipe software VK encoder, PSNR > 30 dB) and T31 Stephane Cerveau (Igalia, NVK video status) — `vulkan.org` events pages
- Vulkan-Video-Samples repo (issue #187: Gen12 AV1 corruption pre-26.1)

**Mesa / drivers / ecosystem (2026)**
- Phoronix *AMD Video Decode Now Unified Between RadeonSI & RADV Vulkan Video* (2026-02-12) → Mesa 26.1-devel; Hawaii/UVD support
- Phoronix *RADV Adds AV1 Vulkan Video Encoding, VCN5 Encode/Decode* (Mesa 25.2, Aug 2026); *Intel …Encode Disable* (Feb 2026); *…Allows Vulkan Video Encode Again* (Jul 2026, Mesa 26.2); *Intel ANV AV1 encode DG2* (Mesa 26.3-devel, Aug 2026); *NVK Vulkan Video Merged For Mesa 26.3*; *Nouveau Patch For Linux 7.3 NVDEC*; *FFmpeg H.265 Vulkan Encode Optimizations* (2026-08-17)
- `github.com/coolsnowwolf/mesa …/docs/drivers/nvk.rst` (+ Vulkanised-2025/web) — NVK 1.4-conformant, Kepler→Blackwell, NVK+Zink
- Mesa release news: *Mesa 26.2.1 released August 20, 2026*; *Mesa 26.1* devel highlight (AV1 decode warmup, Wa_1508208842)
- Intel community forum: *ffmpeg h264_vulkan/hevc_vulkan encode fails on A750* (Dec 2024, `-729850096`)

**Encoder quality**
- IEEE 10937033 *UHD Video Encoding in CPU Versus GPU: Quality and Performance* (BD-rate: GPU needs >20% more bitrate for H.264/AV1)
- NVIDIA *Encoding/Decode Video Codec* benchmark PDF (P1–P7 / VBR-QP-caps, CBR ignores min/max QP)

**Qt**
- `doc.qt.io/qt-6/qrhiswapchain.html` (`PresentMode::NoVSync`→IMMEDIATE, `MinimalBufferCount`, `UsedAsTransferSource`)

---

# Part III — R11–R20 deep-dive (added 2026-09-12)

## 21. How rounds eleven–twenty ran

R11–R20 went one tier deeper than R1–R10: instead of "what exists", these rounds
asked "what does the code actually do, and what does a Nova compositor/encoder
look like once you have a working decode session". Sources were still web +
authoritative code (`FFmpeg master` fetched as source, Khronos spec pages,
Vulkan-Guide/Vulkan-Tutorial, VK-GL-CTS README), but now cross-checked against a
**live RTX 5070 Ti locally** (`vulkaninfo --summary`/`--json`, `ffmpeg n9.0.1`
encode smoke tests). Quote discipline from Part I/II still applies — every fact
below was either read from a fetched source or measured on this machine.

## 22. R11–R20 round-by-round findings

### R11 — Memory & external-memory (where the pixels physically live)

Extensions that gate the zero-copy story, all default-enabled by FFmpeg when
present (`hwcontext_vulkan.c`):

- `VK_KHR_external_memory_fd` — POSIX-fd sharing across processes/devices; the
  Linux staple (`docs.vulkan.org/tutorial/latest/ML_Inference/Third_Party_Libraries/07_gpu_resource_sharing.html`).
- `VK_EXT_external_memory_dma_buf` — Linux `dma_buf` import/export; the Vulkan
  guide explicitly calls it out as "particularly useful … when working with
  hardware video decoders". Ratified, #126, rev 1 (James Jones/NVIDIA, Chad
  Versace/Google, Jason Ekstrand/Intel).
- `VK_EXT_image_drm_format_modifier` — ratified #159, rev 2 — lets a dma-buf's
  per-plane layout (`VkSubresourceLayout2KHR` offset/`rowPitch`) be described
  into a `VkImage` created with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`.
- `VK_EXT_memory_budget` — `VkPhysicalDeviceMemoryBudgetPropertiesEXT`
  `heapBudget`/`heapUsage`: the direct replacement for `cudaMemGetInfo` that the
  Nova `vram_leak` gate needs. Present on this NVIDIA driver (rev 1).
- `VK_EXT_external_memory_host`, `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd`.

What FFmpeg actually does with these (`/tmp/opencode/hwcontext_vulkan.c`):

- **Dedicated allocation is forced for exportable images.** `export_requires_dedicated`
  is set from the DMA-BUF/OPAQUE path by checking
  `VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT`; image creation then always
  uses `VkMemoryDedicatedAllocateInfo` (dedicated logic around line 2474).
- **DMA-buf import** uses `VkImageDrmFormatModifierExplicitCreateInfoEXT` +
  `VkExternalMemoryImageCreateInfo` + `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`,
  with per-plane `VkImportMemoryFdInfoKHR` on duplicated fds and
  `VK_SHARING_MODE_CONCURRENT` image creation; `VkDrmFormatModifierPropertiesEXT`
  consulted at line 209.
- **dma-buf ↔ Vulkan implicit-fence bridging** is done in userspace via the
  ioctls `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` / `DMA_BUF_IOCTL_IMPORT_SYNC_FILE`
  (`struct dma_buf_export_sync_file`, lines 3812 / 4530): the kernel's implicit
  fence is imported into a **binary** semaphore with
  `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` so the Vulkan queue orders
  against whoever produced/consumes the buffer.
- **CUDA interop** (`cuImportExternalMemory`, line 2397) — the current Nova GPU
  path could co-exist with a Vulkan path by importing the same dma-buf; not
  planned, recorded as an escape hatch.

Measured on this machine (NVIDIA 615.71.09): `memory_budget` rev 1,
`external_memory_dma_buf` rev 1, `external_memory_host` rev 1,
`image_drm_format_modifier` rev 2, `external_memory_fd/semaphore_fd/fence_fd` rev 1 —
the full external-memory set is available.

### R12 — Synchronization (the interop seam you will actually write)

Decode/encode submits in FFmpeg **only** use the new pipeline:
`VkSubmitInfo2` → `vkQueueSubmit2`, image barriers are all
`VkImageMemoryBarrier2` (sync2). Video-specific bits:

- Stages/accesses: `VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR`,
  `VK_ACCESS_2_VIDEO_DECODE_READ/WRITE_BIT_KHR` (encode equivalents exist).
  FFmpeg transitions from the image's *current* layout (tracked per-frame;
  starts `VK_IMAGE_LAYOUT_UNDEFINED` on first use).
- Decode layouts: `…_DST_KHR` (output / pool), `…_SRC_KHR` (bitstream-ish
  surfaces), `…_DPB_KHR` (reference images). The internal DPB pool is
  transitioned **once, eagerly** (layered case `vulkan_decode.c` ~1390) to avoid
  per-picture pool transitions; single-picture DPB images transition on first use.
- The "Spec, 07252 utter madness" comment sits exactly where output images flip
  between `VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR` and `…_DPB_KHR` per caps.

**Timeline semaphores are the contract, not fences.** Each AVVkFrame carries one
**timeline** semaphore per image (`VK_SEMAPHORE_TYPE_TIMELINE`, `vulkan.c` 2736).
The FFmpeg exec context (`FFVkExecContext`):
`ff_vk_exec_start` claims the context by bumping a timeline value (`e->sem_value++`,
comment: "the semaphore stays below this value until submission completes …
keeping every other user away"); for every image a frame uses it records
**wait on `sem[i]` @ `sem_value[i]`** and **signal at `sem_value[i]+1`**; after a
successful `QueueSubmit2` it bumps the CPU mirrors (`*sem_sig_val_dst[i] += 1`,
`vulkan.c` 1040). Result: a Nova consumer on the graphics queue waits
`vkWaitSemaphores({sem[i]}, sem_value[i])` (or feeds it into its own
`VkSubmitInfo2.pWaitSemaphoreInfos`) and is ordered exactly after that frame's
decode — no fences, no device sync, no bus polling.

After submit FFmpeg writes the resulting image state back into
`vkf->layout[i] / access[i] / queue_family[i]` (`ff_vk_exec_update_frame` +
submit loop). So the compositor acquiring an NV12 frame **reads
`vkf->layout[0]` and must write back whatever layout it leaves the image in**
— that is the one "API contract" that must not be violated.

**Queue-family answer (critical):** `hwcontext_vulkan.c` (re)builds
`p->img_qfs` from **every distinct queue family the app registered in
`AVVulkanDeviceContext.qf[]`**, and when more than one family is present the
frames' images are created with `VK_SHARING_MODE_CONCURRENT` +
`pQueueFamilyIndices=img_qfs` (lines 2784–2812, 3144, 3572). Registering
graphics, compute, video-decode **and** video-encode families in the shared
device → every decoded image is CONCURRENT-accessible from both the composite
queue and the encode queue with **no queue-ownership transfer**. This is the
single fact that makes the whole P-C→P-D→P-E chain one seamless device.

### R13 — FFmpeg Vulkan hwframes & exec-context internals (beyond R1)

- Decode output image usage: `VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
  (frames-ctx usage & VIDEO_DECODE_DPB)`; the bitstream buffer is a
  `VkBuffer` with `VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR`, size aligned to
  `caps.minBitstreamBufferSizeAlignment` (`FFALIGN`), flushed via
  `FlushMappedMemoryRanges`. Requesting `AV_HWFRAME_MAP_READ` on the frames
  context adds `SAMPLED | TRANSFER_SRC` to the images so the compositor can
  sample decode output directly (no copy).
- Out-of-place DPB: the new `FFVkVideoDPB` refstruct pool holds up to
  `caps.maxDpbSlots` images; **AV1 and any decoder whose dedicated-DPB cap is
  unset force out-of-place** (internal DPB pool), the rest run in-place
  (output == DPB slot). Layered DPBs use one many-layer image. DPB images
  inherit the session's pNext (`VkVideoProfileListInfoKHR`) and, when the
  output pool uses DRM modifiers, the `VkImageDrmFormatModifierListCreateInfoEXT`
  pNext with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`.
- Encode-side: on an IDR FFmpeg writes SPS/PPS/AUD itself into the head of the
  output bitstream (`write_sequence_headers`, `vulkan_encode_h264.c` 1243+),
  advancing `dstBufferOffset` — the marker-usable `units` option
  (AUD / SEI identifier / recovery / timing / A53 CC) is real and surfaced in
  the encoder options table.

### R14 — Compositing & spatial ops (the pipeline shape, decided)

Facts that pin the design:

- `VK_KHR_sampler_ycbcr_conversion` is core 1.1 and **required for any 1.4
  conformant implementation** (per the extension page) — i.e. every 1.4 driver
  must have it, including this NVIDIA one.
- **The conversion is fixed at pipeline creation** ("conversion must be fixed by
  use of a combined image sampler with an immutable sampler in
  `VkDescriptorSetLayoutBinding`") — you cannot swap ycbcr conversions between
  draws; your pipeline+DSL is bound to one conversion. `combinedImageSamplerDescriptorCount`
  for a planar NV12 view can be 1, 2 **or 3** descriptors per driver.
- **QRhi does not expose ycbcr conversions.** `QRhiTexture::createFrom` can wrap
  a native `VkImage`, but the qualifiers you need (mult-plane image view +
  immutable ycbcr sampler) are not expressible through
  `QRhiTextureUploadDesc`/`QRhiSampler`. Net: **you cannot sample a decoded
  NV12 image through QRhi.** This is the decisive R14 finding.

Consequence, now locked as design:
- The decode→viewer and decode→export paths both route through **our own Vulkan
  compute compositor** that reads the NV12 planes (`vkCmdDispatch`, per-plane
  `VK_IMAGE_VIEW_TYPE_2D` luma/chroma plane views, manual BT.601 yuv→rgb — the
  exact law in `core/include/canvas/core/gpu/colorspace.hpp`, matching what the
  CUDA `nv12GradeResize` kernel does), applies scale/transform/rotation/anchor/
  blend-mode/opacity, and writes **one RGBA image** (present) or the **NV12
  encode-input image** (export, parity with `rgbaToNV12`/`nv12Resize`).
- The **QRhi/ViewerGL side only ever samples RGBA** — exactly today's CUDA
  situation (viewer uploads an RGBA texture). QRhi stays the present queue; the
  RHI never touches planar surfaces. Zero-copy is kept decode→composite→encode,
  everything on one `VkDevice`, all releases serialized by the frame timeline
  semaphore; the RGBA surface is the only extra hop and it is the same hop the
  CUDA path takes today.
- Chroma siting / 601-coefficient parity vs the evaluator: the ycbcr-conversion
  sampler path is *optional* (a "use the driver's conversion" fast lane) and
  gated by a byte-parity check against `colorspace.hpp` like `gpu_grade`'
  pins; the default is the manual-matrix path because it is the only one whose
  rounding is *ours*.

### R15 — Encode structure (what a real GOP/keyframe session looks like)

From `vulkan_encode.c` / `vulkan_encode_h264.c`:

- `VkVideoEncodeInfoKHR` anatomy: `srcPictureResource` (input image view +
  `codedExtent` + `baseArrayLayer`), `pSetupReferenceSlot` (current output slot),
  `pReferenceSlots` (all L0/L1 references, then the current slot with
  `slotIndex=-1` "unreferenced-out"), `dstBuffer` (bitstream VkBuffer +
  offset/range), `precedingExternallyEncodedBytes`.
- **The GOP decision is ours, not the driver's.** FFmpeg's base encoder
  produces picture types I/P/B + IDR; `type==FF_HW_PICTURE_TYPE_IDR` triggers
  `write_sequence_headers` (SPS/PPS/AUD) and — for H.264 — `IdrPicFlag` +
  `STD_VIDEO_H264_PICTURE_TYPE_IDR`. Reference assignment / DPB discarding
  (drop references not in the new picture's DPB) is computed FFmpeg-side and
  declared per picture via `VkVideoReferenceSlotInfoKHR`. There is **no
  "GOP size" knob** — you pick I/P/B placement and the reference lists, the
  driver encodes them. `FF_HW_FLAG_NON_IDR_KEY_PICTURES` exists for AV1-style
  new-GOP-without-DPB-reset keyframes.
- Rate control: `VkVideoEncodeRateControlInfoKHR` (CBR/VBR; FFmpeg nudges
  auto): `VkVideoEncodeRateControlLayerInfoKHR` per layer; quality level via
  `VkVideoEncodeQualityLevelPropertiesKHR` after
  `GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR` (bounded by
  `caps.maxQualityLevels`).
- **Intra refresh & quantization maps are extensions FFmpeg does not drive
  (yet):** `VK_KHR_video_encode_intra_refresh` (rev 1) and
  `VK_KHR_video_encode_quantization_map` (rev 2) are *present on this NVIDIA
  driver*, so intra-refresh streaming (every frame a keyframe-candidate) is
  physically possible — but FFmpeg n9 has no knob, so Nova can't use it through
  the encoder without raw `Vk` calls (a P-F spike item, unchanged).
- Budget: per the Khronos "Vulkan Video core API" deck, a video session may
  need 3–8 input/output images **and 4–16 references = hundreds of MB**, and a
  single frame's bitstream can exceed 2 MB. The Nova GPU budget must treat the
  encode path as a ~512 MB allocation class, not a scratch buffer.

### R16 — Seek / random access (why scrub-forward still works)

- Reference flush = **`CmdControlVideoCodingKHR` with `VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR`**
  in a Begin/End batch — FFmpeg's `decode_reset()` does exactly this, then
  waits and re-creates session parameters if `VK_EXT_video_maintenance_2`
  isn't around (`vulkan_decode.c` 370–469, invoked at init and on reset
  boundaries).
- IRAP / keyframe pictures are sync points the driver serves natively; the
  *Nova* seek heuristic is unchanged from the CPU/CUDA path: on seek, decode to
  nearest previous keyframe then step forward (that is what `TimelineDecoder`
  does today via `VideoDecoder::seek_to_frame`). On the Vulkan side a seek is
  `decode_reset` + first keyframe decode — no DPB save/restore, no session
  teardown. Since Nova keeps **one** shared video session and the scrub-preview
  low-res LRU reuses the same worker, there is exactly one session lifetime to
  manage.
- The scrub-preview low-res path (kPreviewMaxDim 640) is untouched by the
  backend swap; it stays a `decode()`-into-tiny-RGBA pipeline, except the RGBA
  write now comes out of the compute compositor.

### R17 — CTS / conformance (what "conformant video" means here)

- Test groups: `dEQP-VK.video.*` with `dEQP-VK.video.decode.*` and
  `dEQP-VK.video.encode.*` sub-trees; sample streams are fetched by
  `external/fetch_video_decode_samples.py` / `fetch_video_encode_samples.py`
  into `external/vulkancts/data/vulkan/video`.
- Run shape: `deqp-vk --deqp-caselist-file=<mustpass>/main/vk-default.txt
  --deqp-log-images=disable --deqp-log-shader-sources=disable` (device selection
  `--deqp-vk-device-id`, fractions ≤16, waivers supported); a build of `deqp-vk`
  is >700 MB before debug info.
- Conformance version is reported per driver via `VkConformanceVersion`
  (`VK_KHR_driver_properties`, core since 1.2) — this RTX 5070 Ti reports
  **1.4.3.3**. Nova does **not** run CTS in CI (huge, slow, sample-stream
  licensing); it consumes the driver-declared conformanceVersion as a gate and
  optionally runs a handful of `dEQP-VK.video.*` smoke cases on the Mesa side
  during vendor bring-up.

### R18 — Present (confirmed against this driver)

Instance-level surface extensions on this machine: `VK_KHR_wayland_surface`,
`VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`, plus `VK_KHR_surface_maintenance1`,
`VK_EXT_swapchain_colorspace`, `VK_EXT_surface_maintenance1`. Device-level:
`VK_KHR_present_id`/`present_id2`, `VK_KHR_present_wait`/`present_wait2`,
`VK_EXT_present_timing`, `VK_EXT_present_mode_fifo_latest_ready`,
`VK_EXT_swapchain_maintenance1`. So everything the Part I "FIFO + present_wait"
decision wanted is measurable on the reference box to a slightly higher
fidelity: `fifo_latest_ready`/`present_timing` exist as a lower-latency
alternative. Under XWayland the Qt app still walks the X11 path; Wayland is
exercised with `QT_QPA_PLATFORM=wayland`. QRhi remains the swapchain owner, so
Nova's present work is: keep MAILBOX-less FIFO, keep `present_wait`, and gauge
whether `present_mode_fifo_latest_ready` is worth a QRhi-backing swap later.

### R19 — Multi-GPU (scope decision confirmed)

- `dma_buf` is a kernel cross-device primitive by design (dma-fence/damage
  tracking); PRIME works between an iGPU+dGPU and between dGPUs, and Vulkan
  external-memory fd sharing works within one device across processes.
- **Cross-GPU video imports are not uniformly supported**: wgpu tracks
  incomplete multi-planar dma-buf import (`wgpu #10059`, `#9801`) — NV12 into a
  *second* GPU is driver/version-dependent. This is exactly the hybrid-laptop
  case (decode on iGPU, present on dGPU).
- Nova's decision does not change: **one physical device owns decode +
  composite + encode**; only the final RGBA ever crosses to a present device,
  and only the encode-side already-running-experimental plan would even try it.
  Device-group extensions (`VK_KHR_device_group*`) are present but not used.

### R20 — Local hands-on probe (RTX 5070 Ti, record)

```
Instance 1.4.357 · surfaces wayland/xcb/xlib · layers: validation, MANGOHUD,
MESA_device_select, NV_optimus, NV_present, OBS, Steam (CI: strip these)

GPU0  NVIDIA GeForce RTX 5070 Ti (Blackwell 0x2c05)
  apiVersion 1.4.351 · driver 615.71.09 · DRIVER_ID_NVIDIA_PROPRIETARY
  conformanceVersion 1.4.3.3 · discrete · NV_optical_flow
  
Queue families (6):
  0: 16 queues  GRAPHICS|COMPUTE|TRANSFER|SPARSE
  1:  2         TRANSFER|SPARSE
  2:  8         COMPUTE|TRANSFER|SPARSE
  3:  1         TRANSFER|SPARSE|*** VIDEO_DECODE ***  → H264 H265 AV1 VP9
  4:  2         TRANSFER|SPARSE|*** VIDEO_ENCODE ***  → H264 H265 AV1 (no VP9)
  5:  1         TRANSFER|SPARSE|OPTICAL_FLOW_NV
  videoCodecOperations decode: H264 / H265 / AV1 / VP9 · encode: H264 / H265 / AV1
  video_maintenance1 + video_maintenance2 · intra_refresh + quantization_map

Video extensions: video_queue rev8, video_decode_queue rev8, video_encode_queue rev12,
  h264_decode rev9, h265_decode rev8, av1_decode rev1, vp9_decode rev1,
  h264_encode rev14, h265_encode rev14, av1_encode rev1
External: memory_fd / semaphore_fd / fence_fd, dma_buf, host, image_drm_format_modifier rev2, memory_budget rev1
Present: present_id(2), present_wait(2), present_timing, fifo_latest_ready, swapchain_maintenance1

Smoke tests (ffmpeg n9.0.1, --enable-vulkan):
  h264_vulkan   640x360 testsrc2 30f → MP4, High profile NV12 @2M, 145 KiB   DECODE-OK (CPU)
  hevc_vulkan   320x180 9f       → 41 KiB                                   DECODE-OK
  av1_vulkan    320x180 9f       → 38 KiB                                   DECODE-OK
  -hwaccel vulkan on the h264 file → silently fell back to CPU (no Vulkan decoders in this build)
```

So: **encode is proven working right now** on Nova's reference box; decode
requires the R1–R10 driver floor (or NVK/Mesa build-out) before a decode smoke
test can light up here.

## 23. Per-vendor additions beyond Part II

| Area | AMD (RADV) | Intel (ANV) | NVIDIA (this box) |
|---|---|---|---|
| External memory | dma-buf + DRM modifiers native | dma-buf + DRM modifiers native | OPAQUE_FD/dma-buf, exposes `image_drm_format_modifier` rev 2 (import semantics unverified — probe before relying) |
| `memory_budget` | yes | yes | yes (rev 1) ✓ |
| `sampler_ycbcr_conversion` | core-required 1.4 | core-required 1.4 | core-required 1.4, confirmed present |
| Video maint1+maint2 | maint1+2 (Mesa track) | maint1+2 | **both present** ✓ |
| Encode surface | H264/H265/AV1 (H265 verify) | H264/AV1 (Mesa 26.2/26.3) | H264 rev14/H265 rev14/AV1 rev1 — **all smoke-validated here** |
| Decode surface | H264/H265/AV1/VP9 | H264/H265/AV1 | H264/VP9 + H265/AV1 (rev 1) |
| Intra refresh / quant-map | see part II floors | see part II floors | **present**, but FFmpeg n9 has no knob (P-F spike) |
| Conformance claim | part II | part II | conformanceVersion **1.4.3.3** local |

## 24. Deltas to the Phase plan P-A..P-G from R11–R20

- **P-A (prober)** — now concrete: run per device a `vulkaninfo --json`-equiva-
  lent audience: queue-family breakdown (video caps + codec ops), conformanceVer-
  sion, video extension set (decode/encode revs + maint1/2 + intra_refresh +
  quant_map), external-memory + memory_budget presence, DRM-modifier import test.
  Add the "RTX 5070 Ti = encode-OK / decode-needs-floor" short-circuit.
- **P-C (decode)** — resolution from R12/R13 is a spec-level patch: share the
  graphics/compute/video-decode/video-encode families in `AVVulkanDeviceContext.qf[]`
  (→ CONCURRENT images, no ownership transfer, R12); set frames-context usage to
  `AV_HWFRAME_MAP_READ`-derived `SAMPLED`; **wait on the frame timeline
  semaphore** at `vkf->sem_value[i]` before composite; hold an `av_buffer_ref`
  of the `AVFrame` during composite so the pool cannot recycle the image; write
  `vkf->layout/access` back after our transition; seek = `CmdControlVideoCodingKHR`
  RESET (R16). The old "DPB/IRAP+CSV" ambiguity hardens into "one shared
  session + reset command".
- **P-D (compositor)** — R14 **revokes** the earlier "sample NV12 through
  QRhi/ycbcr" notion: QRhi cannot do immutable-ycbcr samplers, so the viewer
  chain is **compute-compositor → RGBA → QRhi sample** (same hop the CUDA path
  takes). The optional ycbcr-conversion fast lane is parity-gated. Scale-law
  parity vs `nv12Resize` and blend/opacity parity vs `visual.hpp` stay the
  acceptance criteria.
- **P-E (encode)** — input is the compositor's NV12 image, CONCURRENT to the
  encode family; I/P/B + IDR placement is ours (`VkVideoEncodeInfoKHR`
  reference slots); rate control per-layer + `qualityLevel`; header units
  configurable; budget the session at hundreds-of-MB (R15). Unchanged on
  this machine: h264/hevc/av1 encode already works (R20).
- **P-F (spikes)** — add: driver-DRM-modifier import probe on NVIDIA (R11);
  parity harness for the compute-matrix vs `colorspace.hpp` (R14); intra-refresh
  direct-`Vk` feasibility if streaming-grade latency is ever required (R15).
- **P-G / timeline** — the reference box can run P-E immediately; P-C decode is
  the pacing item on all three vendors (Part II floors). No other change.

## 25. Risk-register additions (R11–R20)

| # | Risk | Weight | Status/response |
|---|---|---|---|
| R-11 | QRhi cannot sample NV12 (ycbcr immutable samplers absent) | high | Compute-compositor→RGBA is now the locked P-D shape; RGBA hop == today's CUDA hop |
| R-12 | NVIDIA exposes `image_drm_format_modifier` but import semantics are unverified | med | P-F probe item: import a decoded dma-buf, byte-compare layout from `VkSubresourceLayout2` |
| R-13 | Decode images must carry `SAMPLED` for composite to avoid a copy | low | Requested via `AV_HWFRAME_MAP_READ` frames-context usage (R13) |
| R-14 | CTS not runnable in Nova CI; "conformant" must come from the driver | low | Gate on `VkConformanceVersion`; optional dEQP `video.*` spot-checks at vendor bring-up |
| R-15 | Encode sessions are memory-hungry (4–16 refs, >2 MB/frame bitstreams) | med | Part of the ~512 MB Vulkan GPU budget; measure with `memory_budget` heapUsage (R11) |
| R-16 | One shared video session + single decoder means seek must use `RESET` | low | `decode_reset` proven path (R16); added to P-C |
| R-17 | Layer ubiquity (MANGOHUD/Steam/OBS/NV_present) skews perf/VRAM numbers locally | low | Strip non-essential layers for perf & `vram_leak` runs (R20 measurement note) |

## 26. Part III references (primary sources for R11–R20)

**FFmpeg source (fetched `master`, 2026-09-12):**
- `libavutil/hwcontext_vulkan.c` — dedicated-alloc/export-requires-dedicated
  (~2474, 2893), DMA-buf import + `VkImageDrmFormatModifierExplicitCreateInfoEXT`
  + CONCURRENT (`VkDrmFormatModifierPropertiesEXT` 209, import 3540–3740),
  CUDA interop (2397), sync-file ioctls (3812, 4530), per-plane fd import (2550+);
  `img_qfs`/CONCURRENT image sharing (2099–2111, 2784–2812, 3049, 3144, 3572)
- `libavutil/vulkan.c` — `VkSemaphoreTypeCreateInfo` TIMELINE (2736, 3538);
  `ff_vk_exec_submit` `VkSubmitInfo2` (991); sem-claim + signal fashions (699,
  769, 921–932); layout/access writeback after submit (1040+); mirror (958)
- `libavcodec/vulkan_decode.c` — video layouts/`…_DST/DPB` "07252 utter madness"
  (508–602), `decode_reset` RESET (370–469), output usage (221), bitstream align
  (291), layered DPB eager transition (1360–1400)
- `libavcodec/vulkan_video.c` — `FFVkVideoDPB` refstruct pool, DPB image creation
  (350–420)
- `libavcodec/vulkan_encode.c` / `vulkan_encode_h264.c` — `VkVideoEncodeInfoKHR`
  anatomy (243+), header pre-write on IDR (1243+, 1287), rate-control structs
  (123–134, 740–770), `qualityLevel` (828–988), ref-slot declaration per picture
  (457–497)
- `libavcodec/vulkan_h264.c`, `vulkan_av1.c`, `vulkan_hevc.c`, `hwcontext_vulkan.c` (DPB/session renames) — per-codec sync-parameter layout

**Khronos spec/registry:**
- `registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_image_drm_format_modifier.html`
  (ratified #159 rev 2; `VkImageDrmFormatModifierExplicitCreateInfoEXT`)
- `registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html`
- `docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_sampler_ycbcr_conversion.html`
  (promotion to 1.1; required for 1.4; immutable-sampler fixity;
  `combinedImageSamplerDescriptorCount`)
- `docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_dma_buf.html`
  (#126 rev 1; dma-buf semantics)
- `docs.vulkan.org/spec/latest/chapters/samplers.html` (+ `…/textures.html`)
- `registry.khronos.org/registry/vulkan/specs/1.3-extensions/man/html/VK_KHR_external_memory.html`
- `docs.vulkan.org/guide/latest/extensions/external.html` and
  `docs.vulkan.org/guide/latest/extensions/VK_KHR_sampler_ycbcr_conversion.html`
  (multi-planar formats, descriptor counts)
- `docs.vulkan.org/tutorial/latest/ML_Inference/Third_Party_Libraries/07_gpu_resource_sharing.html`
- Khronos *Vulkan Video core API* deck (Apr 2021; 3–8 images, 4–16 refs,
  >2 MB bitstream/frame) — `khronos.org/assets/uploads/apis/Vulkan-Video-Deep-Dive-Apr21.pdf`

**CTS:**
- `github.com/KhronosGroup/VK-GL-CTS` + `external/vulkancts/README.md`;
  `mustpass/main/vk-default.txt`; `fetch_video_decode_samples.py` /
  `fetch_video_encode_samples.py`; `--deqp-vk-device-id`, `--deqp-fraction`,
  `--deqp-waiver-file`; `docs.vulkan.org/guide/latest/vulkan_cts.html`
  (conformance via `VK_KHR_driver_properties` `VkConformanceVersion`)

**Interop across GPUs:**
- `android.googlesource.com/device/generic/vulkan-cereal/…/VK_EXT_external_memory_dma_buf.txt`
- `kernel.org/doc/html/v5.13/driver-api/dma-buf.html` (dma-fence/resv;
  PRIME; cross-driver implicit fences)
- `github.com/gfx-rs/wgpu/issues/10059` (+ `#9801`) — multi-planar dma-buf
  import gaps in a second device

**Local machine evidence (measured 2026-09-12):**
- `vulkaninfo --summary` / `--json` (instance 1.4.357; RTX 5070 Ti 0x2c05,
  driver 615.71.09, conformanceVersion 1.4.3.3; queues 16/2/8/1/2/1 with
  decode(H264/H265/AV1/VP9) at qf3 and encode(H264/H265/AV1) at qf4; extension
  revisions as in §22.R20)
- `ffmpeg n9.0.1` smoke encodes (h264_vulkan / hevc_vulkan / av1_vulkan →
  CPU-decode-verified); `-hwaccel vulkan` reports no Vulkan decoders in this build

---

# Part IV — R21–R50 deep-dive: ecosystem, driver bugs, clients, quality (added 2026-09-12)

## 27. Why this pass and how it ran

Part III pinned the API, the shared-device seam, and this machine. This pass set out
to "fill in every gap" about Vulkan Video **as it behaves in the wild** — the bugs
drivers and frameworks actually hit, who ships what, which GPU families matter, and
how far *encode quality* lags the decode API. Method: ~30 search-driven evidence
rounds in five waves — (a) FFmpeg issue trackers + the vulnerabilities touching the
Vulkan decode path, (b) per-vendor driver trackers (AMD/Intel/NVIDIA, plus
Turnip/ARM/mobile), (c) the consumer client projects (GStreamer, VLC, Chromium/ANGLE;
mpv was covered in Part II), (d) official Khronos tooling + validation layers,
(e) encode-side quality/rate-control reports. Primary-source URLs are inline below;
every number that appears names its round.

Three findings re-set expectations early and thread through everything:

1. **Everything shipping today reliably is the *decode* half; the encode half is a
   year-range younger, and its official sample still documents corrupted output.**
2. **No driver passes 100 % of the conformance vector suites** (Fluster), and Intel
   is dramatically the worst (AV1 decode 0/242 on the 2025 run) — decoder correctness
   must be proven per driver, never assumed from "spec conformant".
3. **The largest Linux consumer (Chromium) still has *not shipped* Vulkan Video
   decode** (tracker open since 2024) and the largest FOSS player (VLC) has never used
   it — reviewer-grade decode is thin, which shapes how Nova should position Vulkan
   vs its existing VA-API path.

## 28. Driver + ecosystem bug harvest (R21–R30)

### 28.1 FFmpeg-side security & registration facts (R21/R22/R24)
- **Vulnerability that touches the Vulkan path:** `CVE-2026-64831` —
  stack-buffer-overflow in FFmpeg's Vulkan HEVC decoder `vk_hevc_end_frame`
  (`vps_num_hrd_parameters > HEVC_MAX_SUB_LAYERS`, CWE-121, critical,
  remote-triggerable). Affects FFmpeg 8.0–8.1.2; fixed in 8.1.3. → The Vulkan decoder
  must only ever run on **trusted / self-encoded** media; pin ≥ 8.1.3 in Nova's build.
- **Distro builds are encode-only.** Measured on this box (n9.0.1, Debian-family):
  `ffmpeg -decoders | grep vulkan` is empty and `-c:v h264_vulkan` as a decoder yields
  `Unknown decoder 'h264_vulkan'`, while `-hwaccels` **does** list `vulkan`. The `D`
  in `-encoders` output (`V....D h264_vulkan`) is the "decode supported" flag on the
  *encoder* descriptor, not a decoder. FFmpeg 8.0 release notes advertise H.264/HEVC/
  AV1 Vulkan decode + VP9 + FFv1/AV1 compute codecs — the feature set exists upstream;
  Debian/Ubuntu simply don't build the decoders into libavcodec, so **Nova must
  self-build with `--enable-vulkan` + the explicit decoders** (§33).
- FFmpeg issues now live on Forgejo (`code.ffmpeg.org`); `git.ffmpeg.org` is
  Anubis-bot-checked (automated fetches block → use the GitHub mirror).

### 28.2 NVIDIA (R28) — this machine is inside the current bug window
- **H.264 Vulkan-decode artifacts on Blackwell** (forum
  `forums.developer.nvidia.com/t/h-264-vulkan-video-decoder-produces-severe-artifacts-on-all-h-264-content/378828`,
  Jul–Aug 2026; reported on RTX 5080 Laptop and **5070 Ti**, driver 610.43.03).
  H.265/VP9 and NVDEC are fine; **AV1 also renders wrong** (Aug 5 follow-up). Chrome
  mirror: `crbug 536046187` "AV1 playback lines/artifacts, driver 610.74".
  NVIDIA confirmation (Aug 14): **the fix lands in a GA driver ~end of Oct 2026.**
  → Our RTX 5070 Ti (615.71.09) is inside the window until then; H.264/AV1 *Vulkan*
  decode on this box must not be trusted for color-critical work through ~Oct 2026
  (NVDEC/VA-API/CPU remain correct) — a concrete, dated gate for implementation order.
- **AV1 *encode* corruption on Blackwell** (May 2026): "Vulkan Video Encode AV1
  corrupts as soon as P-frames are enabled… the only clean path is I-frames only
  (idrPeriod=1)." → Long-GOP AV1 Vulkan encode on NVIDIA is not production-usable;
  keep GOP=I-only or route AV1 elsewhere until independently proven.
- Historical floor stands: NVIDIA shipped Vulkan decode first (Win 458.17 / Linux
  455.50.12, since 2021-04) — but the 610+ train regressed H.264/AV1 publicly.

### 28.3 AMD / RADV (R26)
- Decode is in stable Mesa by default (`video_decode` gate became default in the
  Mesa 23.1–24.1 range); newer `RADV_PERFTEST=lowlatencydec/lowlatencyenc` (Mesa 26.1)
  enable the latency-reduced feedback relevant for realtime workflows.
- **RDNA4 / VCN5 confirmed live:** an RX 9070 on Mesa 25.1 exposes **no** `VK_KHR_video_*`
  (Gentoo report Jul 2026; same shape as mesa issue 13118). RDNA4 Vulkan Video lands
  with Mesa 25.2. → PROBE the extension list, never key off the GPU family.
- ICD-shadowing gotcha: with AMDVLK + RADV installed the loader picks the ICD whose
  file sorts first; users fix with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json`
  (the resolution of that same thread). Keep it as a field diagnostics note.
- Mesa floors (Igalia matrix, §30 source): encode H.264/H.265 24.1.0 (RADV MR), AV1
  encode 🚧 (not a stable target); decode H.264/H.265 23.1.2, AV1 24.0.3, VP9 25.2.

### 28.4 Intel / ANV (R27 + Phoronix 2026-07)
- **Wa_1508208842:** Gen12 (TGL/DG1/RKL/ADL) needs a "dummy workload" before AV1
  decode or the AVP unit corrupts output (Mesa MR 39604 / commit 260908ce; tracked in
  Vulkan-Video-Samples #187). Class of bug CI on a new iGPU will not catch.
- Encode: H.264/H.265 encode was **disabled early 2026 "due to insufficient testing"**
  then re-enabled ~July 2026 (incl. Alchemist fixes); **10-bit H.265 encode wasn't even
  implemented until Mesa 26.2** ("Realized 10-bit H265 encoding is not implemented
  yet"); **AV1 encode merged into the Mesa 26.2 cycle (Aug 2026)**.
  → Intel Vulkan *encode* becomes a credible target only from Mesa 26.2.
- Decode extends deeper: H.264/H.265 from Mesa 23.1.2, AV1 from 25.0.0.

### 28.5 Mobile / ARM / others (R29)
- **Turnip (Adreno): no Vulkan Video at all** — feature request open since Feb 2026
  (mesa-for-android-container #28); Qualcomm's supported-extensions doc lists no
  `VK_KHR_video_*`. PanVK (Mali): none; Android viability contested in community.
  Venus (virtio paravirt): none. Zink: H.264 *decoder* WIP only. NVK: H.264/H.265/AV1
  **decoders** (2025), **no encoders**.
  → Vulkan Video is a desktop-Linux + Windows story; "all GPU types" for Nova = the
  three desktop IHVs (+ NVK-on-NVIDIA as a future).

## 29. Client adoption: who actually ships Vulkan Video (R31–R35)

| Client | Decode (h264/h265/av1/vp9) | Encode | Notes |
|---|---|---|---|
| **FFmpeg** | ship (6.1/6.1/6.1/8.0), distro-stripped | ship (7.1/7.1/8.0) | the engine Nova reuses (§28.1) |
| **GStreamer** | h264+h265 by 1.24; av1+vp9 by **1.28** (Jan 2026) + 10-bit h265 | h264 by 1.28; h265/av1 WIP MR | lags FFmpeg by a full feature generation |
| **mpv** | FFmpeg-driven; **prefers VA-API over Vulkan by default** (mpv #18043, May 2026) | — | the canonical `-hwdec=vulkan` field guide (mpv FAQ #13909) |
| **VLC** | **never** — decode stays NVDEC/VA-API/MediaCodec | — | GSoC 2026 = hw-decode→Vulkan-render interop (libplacebo) + external-renderer mode; pattern mirrors Nova's shared-device seam |
| **Chromium/ANGLE** | **not shipped** — bug 324003973 open since Feb 2024 (+43; NVIDIA offered help Nov 2025); Linux decode remains VA-API | — | Khronos v1.0 design doc (Dec 2025) shows the intended shape: Chromium parsers + ~500–700 LOC/codec conversion to StdVideo |
| **Vulkan-Video-Samples** | all four, via `VK_KHR_sampler_ycbcr_conversion` | h264/h265/av1, "issues such as missing POC numbers and corrupted frames" | the official CTS-encoder-backed library; encode sample self-documents immaturity |

Two structural lessons for Nova:

- **The decode contract is confirmed by the Chromium design doc's resource table:**
  VA-API drivers parse SPS/PPS internally; Vulkan Video requires the application (in
  our case FFmpeg's libavcodec) to hand over parsed StdVideo structs. Nova sits on the
  FFmpeg side, so that work belongs to FFmpeg — consistent with Part II's "share
  FFmpeg's decode, write our own compositor" split. Nothing suggests Nova needs to own
  a bitstream parser.
- **Every serious client treats Vulkan decode as optional-behind-flags or not at all;**
  the production Linux default is VA-API. Nova should keep VA-API decode working and
  treat Vulkan as the zero-copy integration / render & encode path for day one, with
  Vulkan decode enabled when the per-driver probe + the §30 checks pass.

## 30. Conformance, validation, robustness (R35/R36)

- **Per-driver conformance is nowhere near 100 %** (Fluster, 2025 runs):

  | Suite | NVIDIA (RTX 4060) | RADV (RX 7600) | Intel (ANV) |
  |---|---|---|---|
  | H.264 `JVT-AVC_V1` | 112/135 | 102/135 | **40/135** |
  | H.265 `JCT-VC-HEVC_V1` | 126/147 | 118/147 | **81/147** |
  | AV1 test vectors | 231/242 | n/a | **0/242** |

  (Vulkan-Video-Samples runs: NVIDIA h265 141/147, av1 231/242; Intel 40/135 / 81/147
  / 0/242.) → Nova's decode gate must be **Fluster-class vectors run through FFmpeg,
  per driver**, not "spec conformant ⇒ correct". Intel 0/242 AV1 is exactly what a
  conformance claim would paper over.
- **Validation layers lag on video objects:** VVL issue #12151 — Vulkan video decode
  crashes with `unique_handles` validation enabled (open). Development rule: leave
  `unique_handles` off around video sessions, or treat its video-object diagnostics as
  suspect. The canonical live reference is now
  `docs.vulkan.org/spec/latest/chapters/videocoding.html` (replaces the older static
  PDF anchors used in Part II; the findings there are unchanged).
- **Timeline-semaphore traps are real and demonstrated in the reference code:**
  KhronosGroup/Vulkan-Samples #588 (Windows out-of-order present → stalled-queue
  deadlock; "present does a full device-wide wait-for-idle") and #1525 (2026-04 race in
  wait-for-next-frame: a preempted worker re-computed a wait value after the semaphore
  had already advanced → permanent hang). This is exactly the R12 risk, hit by the
  Khronos samples themselves. Multi-thread wait-for-next-frame with a shared,
  mutating frame counter is the pattern to avoid in the playback worker.

## 31. Encode maturity — the honest picture (R43–R50)

Developer testimony (GStreamer/Mesa authors, 2024): "hardware crashes; behavior varies
by vendor; rate control and quality issues; synchronization: major issues with both
decoder and encoder." Decode has been stable for years; **encode is where the vendors
still disagree**, and the evidence stack agrees:

- **No production AV1 encoder on Vulkan anywhere yet.** Igalia 2025: RADV AV1 encode 🚧;
  ANV AV1 encode only just merged (Mesa 26.2, Aug 2026); NVIDIA Blackwell AV1 corrupts
  with P-frames (§28.2); the official sample is unresolved. Nova should assume
  **H.264 and H.265 encode qualify; AV1 Vulkan encode does not** — fall back to
  NVENC/VA-API/CPU for AV1 from day one.
- **H.264/H.265 Vulkan encode is the stable floor** (FFmpeg 7.1+, Mesa/NVIDIA/AMD GA),
  with the standing caveat that quality ≈ vendor rate-control, not x264 — Nova's export
  presets must own targets/rates/QP, and "GPU-native" ≠ "visually identical".
  Conformance is decode-side; encode is judged by decode-ability + metrics, which is
  what Nova's export round-trip tests already do.
- **10-bit:** decode is universal; **10-bit *encode*** is the drift point — ANV only
  got H.265 10-bit in Mesa 26.2 (Jul 2026), and AV1 10-bit encode is driver-dependent.
  Keep 10-bit encode an opt-in probed per driver extension list, not a menu entry.
- **NVIDIA H.264 decode-as-a-consumer is safe but needs the Oct-2026 driver gate**
  (§28.2); until then the probe should still prefer NVDEC/VA-API for H.264/AV1 input.

## 32. What Vulkan Video still is not (R36–R40) + platform matrix

Codecs **not** in the API (checked against the live video-coding chapter and the
Khronos announce series): MPEG-2, MPEG-4 ASP, VC-1, VP8, MJPEG, VVC/H.266, and
(trivially) the FFmpeg *compute* codecs — FFv1 / ProRes RAW ride the compute path, not
video queues, so `-c:v ffv1_vulkan` is architecture, not a video extension. Beyond
codecs:

- **Interlaced: decode-only** (interleaved + field pictures); no interlaced encode —
  consistent with Part II's scope call for m2t.
- **AV1 film grain:** the parameters ride StdVideo structs and the decoder applies them;
  grain-on-Vulkan has had per-driver regressions. Treat grain as a per-driver decode
  correctness item with a CPU fallback, never a guaranteed feature.
- **Subtitles / CEA / ancillary data:** entirely outside Vulkan Video; stays in
  FFmpeg-domain (containers/captions untouched by the Vulkan path).
- **Protected content:** possible via protected buffers (Chromium's design doc calls
  the Vulkan flow *simpler* than VA-API's decrypt-during-decode) — Nova has no DRM
  scope, so this is informational only.
- **Encoders offer no rate-distortion estimation and no bitstream-format writing:** the
  caller owns RC config and the AVCC header/SEI composition (via FFmpeg) — exactly the
  contract the export path already assumes.

Platform matrix ("all GPU types", with the driver maturity from §28 in play):

- **Linux desktop (Nova's target):** proprietary NVIDIA (full matrix, §28.2 caveats),
  RADV/ANV (decode solid, encode ≥ Mesa 26.2 for Intel), NVK (decode only). Linux =
  the only fully-open stack.
- **Windows:** all three IHVs ship decode + encode (Igalia floor table); Chromium-class
  consumers are flag-gated. Informational only for Nova.
- **Android / mobile / ARM:** MediaCodec owns the pipeline; Turnip/PanVK/Venus have no
  Vulkan Video (R29). Not a Nova target.
- **macOS / iOS:** no Vulkan at all (Metal); MoltenVK has no video support.
- **Licensing:** the extensions and StdVideo headers are Khronos-licensed like the rest
  of Vulkan; no patent obligations beyond the codec pools the caller already deals with
  (H.264/HEVC/AV1 are codec-level, orthogonal to the API). No action for Nova.

## 33. Deltas to the plan + risk-register additions (R21–R50)

Deltas that change the Part III plan:

1. **Add a dated gate for H.264/AV1 Vulkan decode on NVIDIA:** ~end of Oct 2026 driver
   (this box: 615.71.09, affected). Until then the P-V aid path keeps NVDEC/VA-API/CPU
   for H.264 and AV1 input; H.265/VP9 Vulkan decode is unaffected.
2. **Do not ship Vulkan *encode* for AV1** (no production encoder, + Blackwell
   P-frame corruption, + ANV only just merged). GPU AV1 export falls back to NVENC /
   VA-API / CPU. H.264/H.265 Vulkan encode stays the qualified path, probed per driver.
3. **Move "keep VA-API decode functional" from a nice-to-have to a hard requirement for
   the Vulkan phase** — every serious client, and Nova's own dev box, needs it as the
   correctness baseline while Vulkan decode earns per-driver trust.
4. **Fluster-class per-driver decode vectors belong in CI** (via FFmpeg, mirroring the
   2025 per-driver scores in §30) — spec claims are not enough; Intel's 0/242 AV1 and
   NVIDIA's §28.2 artifacts are the evidence.
5. **The Vulkan-FFmpeg build is a self-build.** Add the `--enable-vulkan` + explicit
   `h264_vulkan/hevc_vulkan/av1_vulkan/vp9_vulkan` decoder flags and ≥ 8.1.3 pin to the
   engine's FFmpeg provisioning; the distro package cannot provide in-process decode.
6. **Interop target set unchanged** (shared device, timeline semaphores, CONCURRENT,
   dma-buf) — confirmed externally by Chromium's design doc and VLC GSoC, both of which
   plan the identical zero-copy decode→render seam. No new API surface found.

New risk-register rows (append to §12 and Part III's §25 table):

| ID | Risk | Evidence | Mitigation |
|---|---|---|---|
| R-18 | NVIDIA Blackwell H.264/AV1 Vulkan decode artifacting until ~Oct 2026 driver | forum 378828; crbug 536046187; this box 615.71.09 | dated probe gate: route H.264/AV1 to NVDEC/VA-API/CPU until driver ≥ fix; retest smoke vectors |
| R-19 | No production Vulkan AV1 encoder (all vendors) | Igalia 2025 matrix; Blackwell P-frame corruption; samples README | never enable av1_vulkan in presets; fall back to NVENC/VA-API/CPU |
| R-20 | Per-driver decode conformance gaps (Intel AV1 0/242) | Fluster 2025 (per-driver) | CI decode vectors per driver; treat known-fail vectors as CV |
| R-21 | VVL `unique_handles` crashes on video objects | VVL #12151 | dev builds: disable unique_handles for video sessions |
| R-22 | CVE-2026-64831 in FFmpeg Vulkan HEVC decoder (pre-8.1.3) | CVE/no fixed milestones | pin FFmpeg ≥ 8.1.3; decode only trusted media |
| R-23 | Intel encode maturity window (10-bit/AV1 only ≥ Mesa 26.2) | Phoronix 2026-07; ANV re-enable | probe `VK_KHR_video_encode_*` rev per driver; 8-bit H.264 floor |
| R-24 | RDNA4/VCN5 video requires Mesa 25.2; extension-probe, not family-probe | mesa issue 13118; Gentoo 9070 report | probe extension list at startup; never key off device name |

## 34. Part IV references (primary sources for R21–R50)

- Igalia "Vulkan Video Ecosystem Status" (live page, last update 2026-05-22):
  `blogs.igalia.com/vjaquez/vulkan-video-status` — per-driver ext-floor table
  (Mesa/NVIDIA/AMD, decode+encode per codec), SDK milestone anchors (1.3.239 decode,
  1.3.275 encode, 1.3.280 AV1 decode, 1.3.302 AV1 encode, 1.4.317 VP9), multi-framework
  framework table (GStreamer/FFmpeg). This is now the single authoritative matrix.
- Vulkanised 2025 "Vulkan Video is Open: Application Showcase" (S. Cerveau, Igalia)
  `vulkan.org/user/pages/09.events/vulkanised-2025/T31-Stephane-Cerveau-Igalia.pdf` —
  Fluster per-driver numbers (§30), driver capability table (§28.3/28.4/28.5), NVK/Zink.
- Vulkanised 2024 "A Vulkan Video Encoder from Mesa to GStreamer" (Hyunjun Ko /
  Stéphane Cerveau) PDF — the encode-side "hardware crashes / vendor variance / rate
  control & quality" testimony (§31).
- Chromium Vulkan Video integration design doc v1.0 (2025-12-04, draft):
  `khronos.org/vulkan/chrome-video/vulkan_video_integration.html` — parser-sharing
  analysis, VA↔Vulkan resource table, ~500–700 LOC/codec conversion estimate,
  dma-buf/protected-content flows (§29).
- Chromium issue 324003973 (Vulkan Video decode feature request, open since 2024-02);
  crbug 536046187 (AV1 artifacts, NVIDIA 610.74).
- NVIDIA forum 378828 (H.264 Vulkan decode artifacts incl. RTX 5070 Ti/5080 Laptop,
  ~end-of-Oct-2026 GA fix, AV1 follow-up); NVIDIA forum Blackwell AV1 encode
  P-frame-corruption thread (May 2026).
- Mesa MR 39604 / commit 260908ce (Wa_1508208842 AV1 decode warmup, Gen12);
  mesa issue 13118 (RDNA4 video gap); Phoronix 2026-07-14 (ANV 10-bit H.265 encode,
  Mesa 26.2); Gentoo forums "Mesa RADV does not provide vulkan video support" (RX 9070,
  Mesa 25.1, ICD-shadowing `VK_DRIVER_FILES` resolution).
- GStreamer 1.28 release coverage (linuxiac, 2026-01-28 — AV1/VP9 decode + H.264 encode
  + 10-bit H.265); mpv issues #18043 / #18104 + FAQ #13909.
- VLC GSoC 2026 (A. Sobhy / Thomas Guillem) — libplacebo-Vulkan hw-decode interop,
  external-renderer mode (`ahmedsobhy.net/blog/my-gsoc-2026-project-overview`).
- KhronosGroup/Vulkan-Video-Samples README ("…issues such as missing POC numbers and
  corrupted frames" — encode sample); Vulkan-Video-Samples #187 (Wa_1508208842).
- KhronosGroup/Vulkan-Samples #588 (Windows timeline-semaphore deadlock) and #1525
  (2026 wait-for-next-frame race).
- KhronosGroup/Vulkan-ValidationLayers #12151 (unique_handles crash on video decode).
- `docs.vulkan.org/spec/latest/chapters/videocoding.html` — live canonical Video
  Coding chapter (usage bits, format-property buckets, session reset semantics).
- CVE-2026-64831 entry (FFmpeg Vulkan HEVC decoder stack overflow, fixed 8.1.3).
- Local re-check (2026-09-12): `ffmpeg -decoders`/`-encoders`/`-hwaccels` on n9.0.1 —
  decode-side Vulkan absent, encode-side present (confirms the §28.1 self-build need).

---

# PART V — The phases (working plan; P-A..P-G expanded, deltas folded in)

This is the *executing* plan. §10 defined P-A..P-G in one line each; §18, §24 and §33
carried the deltas. This part consolidates all of it into the phase shape Nova actually
builds by: **objective → tasks → tests → exit gate → driver floors**. Every phase keeps
the CPU/VA-API/GL parity path running — Vulkan is additive until P-G flips the default.

**Ground rules carried from Parts I–IV into every phase below:**
- Shared-device strategy is locked: one `VkInstance`/`VkDevice`; graphics, compute,
  video-decode and video-encode queue families all registered in `AVVulkanDeviceContext.qf[]`
  (→ `VK_SHARING_MODE_CONCURRENT` images, no ownership transfer) (R2/R12).
- Timeline semaphores are the contract; **wait at `vkf->sem_value[i]`** before composite,
  hold an `av_buffer_ref` of the `AVFrame` during composite, write `vkf->layout/access`
  back after our transitions (R12/R13).
- Frameworks layer-free for perf/VRAM runs (MANGOHUD/Steam/OBS/NV_present skew — R17).
- Extension-probe devices, never GPU-family-probe (R24); self-built FFmpeg ≥ 8.1.3 with
  explicit decoders (CVE-2026-64831 — R22, §28.1); VA-API decode stays functional as the
  correctness baseline until Vulkan decode earns per-driver trust (§29, §33.3).
- VVL `unique_handles` disabled in dev builds for video objects (VVL #12151 — R21).
- No `av1_vulkan` encode, ever (R19, §33.2); H.264/H.265 Vulkan encode only behind
  per-driver probe + driver-floor gates.

---

## Phase 0 — Vulkan-tests (test-first; blocks everything)

> User directive: *start with tests; a new folder called `Vulkan-tests`; rebuild every
> single test we have with Vulkan if it's OpenGL, and build and test everything before
> we start Phase 1 (P-A).* Everything in P-A..P-G lands on top of a suite that already
> proves the backend — so the harness and the parity tests exist *before* the code does.

**Objective.** Stand up the Vulkan test suite *first*. Every existing test that touches a
GPU/GL path gets a Vulkan reading run on the same input through the same law; every
headless test is re-run as-is. Nothing else starts until the whole tree builds clean and
all tests pass on the Vulkan harness.

**New folder:** `Vulkan-tests/` at repo root (canonical recipe mirrors `core/tests/`,
`gui/tests/`, `gui/tests/qt/` — see AGENTS.md "Adding a new headless module"). Wired as
its own CMake target tree so `ctest --test-dir build` gains a `vulkan/` group; headless
targets keep the no-Qt link rule and `check_qtdep.sh` coverage.

**Tasks**
1. **Probe/selftest harness** — `Vulkan-tests/vk_probe_test`: instance ≥ 1.3; per physical
   device dump of conformance version, `videoCodecOperations` per queue family, video
   extension set (decode/encode revs + maint1/2 + intra_refresh + quant_map), `memory_budget`
   presence, DRM-modifier import test. Driver-floor short-circuit for "RTX 5070 Ti =
   encode-OK / decode-needs-floor" (§18 P-A). Skip machinery: a device without a codec op
   reports `SKIP`, never FAIL.
2. **GPU/GL test mirrors** — rebuild every test that exercises a GPU/GL path as a Vulkan
   reading. Concrete targets (each = one `<name>_vulkan` test in the new tree):
   - `gpu_grade` → `gpu_grade_vulkan` — the fused composite kernel law mirrors to a
     Vulkan compute kernel; host-side mirror pins identity / limited-range / CPU-export
     byte-parity + flip/scale/opacity "changes pixels" checks (§11). Gate is byte-
     identical, not spectral (§11; same discipline as the CUDA `nv12GradeResize` pin).
   - `visual_render` → `visual_render_parity` — identity render byte-identical across
     CPU, GL, Vulkan; synthesis + flip/scale/opacity deltas (§11).
   - `vram_leak` → `vram_leak_vulkan` — synthetic 2K H.264, Vulkan-decoded at native res,
     composited to present, host-download samples, steady-state VRAM drain < 256 MB via
     `VK_EXT_memory_budget`/`vkGetPhysicalDeviceMemoryProperties` (§11; mirrors the NVDEC
     gate that pinned the `av_frame_ref`-onto-dirty-`retain_hw_` leak). Layers stripped
     (R17). SKIP when `memory_budget` absent.
   - `scrub_bench` → `scrub_bench_vulkan` — p95 scrub-preview latency vs NVDEC (§11).
   - `export_sweep` → encoder sweep extended with `h264_vulkan`/`hevc_vulkan` (§10 P-E,
     §11) — verified by ffprobe + decode-roundtrip; every valid `deliver_settings_model`
     container × codec combo (§11). `av1_vulkan` rows run **decode-only / probe-only**.
3. **Headless re-run** — `roundtrip`, `colorsci`, `graph/_edit`, `composite`, `op`, `lut`,
   `wheels_ui`, `curves`, `histogram`, `equalizer` (known-red WIP — mark, don't gate),
   `clip_rate`, `voice_isolation`, `timeline_decoder`, `audio_pipeline`, `sonicsync`,
   `av_reanchor`, `transition_bake`, `timeline_snap/_selection/_drag`,
   `transition_handle_editor`, `audio_targets`, `timeline_volume_line`,
   `deliver_settings_model`, `source_preview_model`, plus the 3 Qt-linked tests
   (`volume_line_drag_qt`, `waveform_placement_qt`, `wheel_panel_roundtrip_qt`) — these are
   backend-agnostic and must still pass unchanged; they are the regression floor.
4. **Fluster-class per-driver decode vectors** — CI vectors per driver via FFmpeg,
   mirroring the 2025 per-driver scores in §30 (R20). Known-fail vectors are recorded as
   expected-fail (CV), never silently dropped (§30, §33.4).
5. **Tooling pin** — Vulkan-Headers via git, generated `vulkan_raii.hpp` bindings, VVL
   version state in CI (§12 API/tooling churn row). FFmpeg provisioning script builds with
   `--enable-vulkan` + explicit `--enable-decoder=h264_vulkan,hevc_vulkan,av1_vulkan,vp9_vulkan`
   and pins ≥ 8.1.3 (§33.5, R22).

**Exit gate (Phase 0 complete).** All of: (a) `Vulkan-tests/` exists with a passing
probe/selftest; (b) every GPU/GL test named in task 2 has a `_vulkan` reading — passing or
SKIP-by-probe, never absent; (c) `cmake --build build -j` (Debug and Release and
`build-release/`) is **warning-free on all targets**; (d) `ctest --test-dir build` =
35/35 (or 35/35 with only the documented `equalizer` WIP red) **plus** the Vulkan group;
(e) `./scripts/check_qtdep.sh` passes. No P-A work is merged until (a)–(e).

**Driver floors for Phase 0:** none beyond "Vulkan 1.3-capable driver present"; each
mirrored test self-gates on its own capability (SKIP without the codec op or
`memory_budget`).

---

## P-A — Runtime & probe

**Objective.** A deliberate, non-intrusive Vulkan runtime: context creation, physical-device
+ queue-family inventory, and a headless selftest. No app behaviour change (§10).

**Tasks**
- `core/src/gpu/vulkan/vk_context.*`; `vk_video_available()` entry point.
- Physical-device report: queue families + `videoCodecOperations`, conformance version,
  `queryResultStatusSupport`, `maxQualityLevels`, video extension set (§18 P-A).
- Validation layers wired in Debug (`VK_LAYER_KHRONOS_validation` video + synval sections,
  §18 P-A / R8); `unique_handles` off for video objects (R21).
- Instance ≥ 1.3 (required for the FFmpeg shared-device rationale, R9).
- Populate `AVVulkanDeviceContext.qf[].video_caps` for every family reporting an op (R2).
- The "RTX 5070 Ti = encode-OK / decode-needs-floor" short-circuit lives here (Phase 0
  probe output consumed by the runtime, §18 P-A).

**Tests.** `vk_probe_test` (Phase 0) now runs against the real `vk_context`; `gpu_grade_vulkan`
host-mirror sanity already passes (proves the compute path in-process).

**Exit gate.** `vk_probe_test` + `gpu_grade_vulkan` green; `vulkaninfo`-equivalent dump
recorded for the reference box; no app code path changed; `check_qtdep.sh` + full prior
suite still green.

**Driver floors.** Instance 1.3; this phase is floor-free (any Vulkan 1.3 driver).

---

## P-B — Viewer swap (`ViewerVk`)

**Objective.** A GL-less escort viewer that presents the existing CPU RGBA frames —
byte-identical pixels, transition shader ported to SPIR-V (§10). Establishes the Qt/RHI
presentation seam (QRhiWidget, `beginExternal`, swapchain mode) without GPU decode yet.

**Tasks**
- `ViewerVk` (`QRhiWidget`) beside `ViewerGL`; same geometry/scale/fit code path re-used.
- Transition shader → SPIR-V via `qt_add_shaders`/`qsb`.
- Presentation: FIFO + `present_wait` everywhere; `VK_EXT_present_timing` escalation only
  when present (§7, §18 P-F, §12 present risk). Audio-master stays the clock (§7).
- Keep Quad/plane-pair path identical to `ViewerGL` for the parity harness.

**Tests.** `visual_render_parity` now includes the **viewer** reading: identity frame via
`ViewerVk` byte-identical to `ViewerGL` and CPU; transition frames match. New
`swapchain_mode_vk` probe records FIFO/`present_wait` availability on the target
compositor (Wayland vs X11 — §13.4).

**Exit gate.** `ViewerVk` presents every `ViewerGL` input byte-identically across a
scrub sequence and a crossfade; parity test green; GL viewer still default.

**Driver floors.** None — a bare 1.3 driver suffices (present path, no video).

---

## P-C — Decode on GPU

**Objective.** `TimelineDecoder` gains a Vulkan decode path using **self-built FFmpeg**
(`h264/hevc/av1/vp9_vulkan`) on the shared device (§18, §33.5).

**Tasks** (spec-level resolution from R12/R13/R16, §24 P-C)
- Request decode images with `SAMPLED` via the `AV_HWFRAME_MAP_READ`-derived frames-
  context usage (R13) so composite reads in place (no copy).
- Wait on the frame timeline semaphore at `vkf->sem_value[i]` before composite; hold an
  `av_buffer_ref` of the `AVFrame` during composite so the pool cannot recycle the image;
  write `vkf->layout/access` back after our transitions; then composite (R12/R13).
- One session per media slot; seek = `CmdControlVideoCodingKHR` RESET (`decode_reset`
  proven path, R16) — the "one shared session + reset command" shape hardens the old
  DPB/IRAP ambiguity (§24).
- Error-concealment hook on `VkQueryResultStatusKHR` (R6); skip/refill strategy for
  corrupt or partial frames.
- Low-res scrub preview built from Vulkan images; CPU + NVDEC/VA-API decode paths remain
  (this is the §33.3 baseline).
- **Dated gate:** on NVIDIA, H.264/AV1 **Vulkan** decode routes to NVDEC/VA-API/CPU until
  the post-fix GA driver (~end of Oct 2026; forum 378828 / crbug 536046187 — this box
  `615.71.09` is affected, R18). H.265/VP9 Vulkan decode unaffected.

**Tests.** `vram_leak_vulkan` (steady-state < 256 MB, §11); `scrub_bench_vulkan` vs NVDEC
(p95); `visual_render_parity` gains the **decode→composite→viewer** chain; Fluster-class
vectors per driver (R20); decode-roundtrip correctness gate (encode→decode→frames + ffprobe).

**Exit gate.** H.264/HEVC test vectors byte-correct on at least two vendors; seek with
session-reset has no `DEVICE_LOST`; `vram_leak_vulkan` steady-state; scrub p95 within
NVDEC budget; no regression in the CPU/VA-API paths (the §33.3 baseline).

**Driver floors.** NVIDIA: decode H.264/AV1 **blocked until the ~Oct 2026 GA fix** (R18);
else fine (H.265/VP9). RADV: decode H.264/H.265 ≥ 23.1.2, AV1 ≥ 24.0.3, VP9 ≥ 25.2,
RDNA4/VCN5 ≥ **25.2** (extension-probe — R24). ANV: H.264/H.265 ≥ 23.1.2, AV1 ≥ 25.0.0,
Gen12 AV1 warmup handled (Wa_1508208842). NVK: decode-only, acceptable as a third vendor
if present.

---

## P-D — Composite in Vulkan

**Objective.** The compute compositor: multi-planar (NV12) sampling + scale + per-clip
transform/blend/opacity + SrcOver, a `frame_gpu` mirror (R14, §24 P-D).

**Tasks**
- **Compute-compositor → RGBA → fragment-sample** is the locked shape: QRhi cannot do
  immutable-ycbcr samplers, so NVIDIA/AMD NV12 stays in compute until RGBA (R14 §24).
  Optional ycbcr-conversion fast lane **parity-gated** — never default.
- Own RGB conversion shader with chroma siting per codec at sample-position choice — not
  fixed-function ycbcr (R3 / R14); keep NV12 through the composite (R3).
- Scale-law parity vs `nv12Resize` (CUDA mirror); blend/opacity parity vs `visual.hpp`
  (`opacity`/`blend_mode` laws, §9.5) are the acceptance criteria.
- Layers stripped for perf/VRAM runs (R17).

**Tests.** `gpu_grade_vulkan` now covers the full composite kernel: bit-exact host mirror
(identity / limited-range / CPU-export), flip/scale/opacity "changes pixels", blend-mode
parity vs `visual.hpp`; `visual_render_parity` extends through the compute path (§11 §24).

**Exit gate.** Compute-compositor output byte-identical to the CUDA/CPU ladder on identity;
scale and blend laws match `nv12Resize`/`visual.hpp` within the existing test tolerances;
no present-path copy regressions.

**Driver floors.** Same as P-C (composite rides the decode images); no new floor.

---

## P-E — Export on Vulkan

**Objective.** `av1_vulkan`/`hevc_vulkan`/`h264_vulkan` registered behind the probe;
RGB→NV12 kernel parity with `rgbaToNV12`; `export_sweep` extended (§10).

**Tasks** (§18 P-E, §24 P-E, R15)
- Encoder input = the compositor's NV12 image, CONCURRENT to the encode family.
- I/P/B + IDR placement is ours (`VkVideoEncodeInfoKHR` reference slots); layered rate
  control + `qualityLevel`; header units configurable.
- Option mapping (`rc_mode=cqp`+`quality` internal; `cbr`/`vbr` deliverable, §16 R5).
- Budget one encode session at hundreds-of-MB (§24 P-E, R15); measure with `memory_budget`.
- **No `av1_vulkan` encode** (R19, §33.2) — AV1 export falls back to NVENC/VA-API/CPU.
- encode→decode→frames + ffprobe round-trip is the correctness gate; PSNR/BD-rate sanity
  only noted in release notes (§18 P-E).

**Tests.** Encoder sweep (§11) — every `deliver_settings_model` container × codec incl.
`h264_vulkan`/`hevc_vulkan`, verified by ffprobe + decode-roundtrip; `av1_vulkan` rows
decode-only.

**Exit gate.** H.264 + H.265 Vulkan encode output decodes cleanly on CPU and NVDEC/VA-API
(hw->hw roundtrip); GOP/I-P-B structure correct per stream; `vram_leak_vulkan` still
steady with an encode session resident; Intel gates honoured.

**Driver floors** (from §24 P-E / §33, R23): AMD RADV ≥ 26.1 decode-latency flags / ≥ 25.2
AV1-encode experimental — **do not ship`av1_vulkan`**; ANV encode ≥ **26.2** (8-bit H.264
floor; 10-bit H.265 + AV1 ≥ 26.2, R23); NVIDIA proprietary encode already good on this box
(`615.71.09` encode-OK, §18).

---

## P-F — Zero-copy end-to-end

**Objective.** decode → composite → encode on one device with no host copies; multi-profile
images (`VkVideoProfileListInfoKHR`, TRANSCODING hint); seek with clean DPB reset; present
on swapchain where caps allow (§10).

**Tasks**
- Multi-profile images + TRANSCODING hint (decode+encode sharing the image).
- Inline-query results (`VkVideoBeginCodingInfoKHR.pInlineQueryResults`) for per-frame
  success (R6).
- Timeline-fence/`present_wait` pacing maintained end-to-end; FIFO everywhere (§18 P-F).
- Driver-DRM-modifier import probe on NVIDIA — import a decoded dma-buf, byte-compare
  layout from `VkSubresourceLayout2` (R11/R12) — the seek/trim path stays CPU-correct
  while the GPU path proves zero-copy.
- Intra-refresh direct-`Vk` feasibility only if streaming-grade latency is ever required
  (R15 — not otherwise a target).

**Tests.** `vram_leak_vulkan` steady-state across full decode→composite→encode resident
set; `scrub_bench_vulkan` (seek with reset) under budget; `visual_render_parity` end-to-end
decode→composite→encode→decode round-trip; `export_sweep` on the shared sessions.

**Exit gate.** Zero-copy round-trip live on the reference box (this machine can run P-E
immediately, P-C decode is the pacing item — §24 P-G); `vram_leak_vulkan` steady-state;
timeline-semaphore hazards absent (Vulkan-Samples #588/#1525 traps acknowledged in the
sync design, R31).

**Driver floors.** Those of P-C + P-E combined (decode floor for the input codec, encode
floor for the output codec).

---

## P-G — Hardening & rollout

**Objective.** Make the Vulkan path the safe default or a documented opt-in, fully gated.

**Tasks**
- Manual zero-warning discipline on **all** Vulkan targets (Debug, Release, `build-release/`);
  optionally promote `-Werror` once Phase 38 of `splitplan.md` lands (§10 P-G).
- `check_qtdep.sh` coverage for every new headless module in `core/src/gpu/vulkan/` and
  `Vulkan-tests/`.
- Per-driver test-matrix gates (`driver-matrix gating` from §11) — encode skips below its
  known-good floor; `-hwaccel vulkan` is **not** default until the driver-floor check
  passes (R10/§18 P-G); optional deep CI runs `dEQP-VK.video.decode.*`/`encode.*` mustpass
  subset when a video-capable GPU is present (§18 P-G).
- Docs: AGENTS.md + ARCHITECTURE.md Vulkan sections, BUILDING.md self-build FFmpeg
  instructions.
- Rollout: optional `CANVAS_VULKAN` default-on or env/CLI toggle (SOC util? explicit
  flag) so the switch is reversible per machine (§10 P-G).

**Tests.** Full `ctest` (35 + Vulkan group) on all three build flavours; CI driver-matrix
run; a manual smoke card: open a 2K H.264 project, scrub, crossfade, export H.265 — all
Vulkan — and eyeball vs the VA-API default.

**Exit gate.** Everything green everywhere; the §29 lesson held (Vulkan decode optional,
VA-API baseline intact); documentation current; rollout toggle shipped.

**Driver floors.** The full tiered gate set from §7/§18/§24/§33 — NVIDIA H.264/AV1 decode
post-~Oct-2026 fix (R18), Intel encode ≥ 26.2 (R23), RADV decode floors, RDNA4 ≥ 25.2
(R24), decode only on trusted media ≥ FFmpeg 8.1.3 (R22).

---

## Phase exit-gate ledger (one-glance)

| Phase | Gate | Blocks |
|---|---|---|
| 0 | `Vulkan-tests/` green; every GPU/GL test mirrored; full build + ctest warning-free | P-A |
| P-A | probe + host-mirror green; no app behaviour change | P-B |
| P-B | ViewerVk byte-identical parity; swapchain probe recorded | P-C |
| P-C | 2-vendor decode vectors byte-correct; `vram_leak_vulkan`; scrub p95 vs NVDEC | P-D |
| P-D | compute-compositor parity vs `nv12Resize`/`visual.hpp` | P-E |
| P-E | H.264/H.265 Vulkan encode round-trips; `av1_vulkan` never offered | P-F |
| P-F | zero-copy round-trip steady-state; no timeline-semaphore hazards | P-G |
| P-G | full suite × 3 build flavours; drivers gated; docs + rollout toggle shipped | production default |
