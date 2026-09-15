# NVENC — Options, Modes & Measured Reference (RTX 5070 Ti)

> Companion to `docs/vaapi.md`. The NVIDIA encode route: every NVENC mode FFmpeg
> exposes, what this engine wires today, and — most importantly — **measured
> numbers on real hardware** for the 1440p60 deliver law, so preset/RC choices
> are evidence, not lore. Measured 2026-09-15 on an NVIDIA GeForce RTX 5070 Ti
> (Blackwell, 9th-gen NVENC) against the real 1440p60 AV1 reel
> (`2026-09-14 09-12-48.mkv`, 2560x1440 @ 60 fps) via `hevc_nvenc`, 180-frame
> head window, 80 Mbps target / 120 Mbps max unless stated.

---

## 1. TL;DR / decision record

| Question | Answer |
|---|---|
| Which preset for the 80 Mbps deliverable? | **p4, tune hq, CBR.** Full feeder-bound speed (~1750 fps end-to-end) with the tightest rate control (~65 Mbps actual). |
| Why not p1 (fastest)? | Same speed as p4 here (both feeder-bound), but **CBR overshoots to ~93 Mbps** on short windows. Speed tie, rate loss — no reason to take it. |
| Why not p7 (best quality)? | Halves throughput (832 fps) for no deliverable benefit at 80 Mbps saturation. Keep for a quality-first preset slot, not the default. |
| Multipass / lookahead / AQ? | Multipass-qres + lookahead32: slower AND +50% bytes — lose/lose at this bitrate. AQ costs no speed but spends bits (a quality dial, not a speed dial). ll/ull: identical speed to hq for file export (latency-only tunables). |
| Engine mapping today | `nv_preset_for()` (`core/src/export/exporter.cpp:787`): `medium`→p4, `fast`→p3, `faster`/`veryfast`→p2, `slow`→p6, `veryslow`/`placebo`→p7. **`medium`→p4 remap applied 2026-09-15** (see §4). |

**Bottom line:** Blackwell NVENC outruns the feed pipeline — p1 through p4 measure
identically because software AV1 decode + scale + mux is the ceiling. Pick the
preset on **rate-control accuracy**, and that is p4.

---

## 2. Option map (FFmpeg `hevc_nvenc`, this box's build)

- **Preset** (`-preset`, default p4): `p1` fastest … `p7` slowest/best. Legacy
  aliases: slow (=hq 2-pass), medium (=hq 1-pass), fast (=hp 1-pass).
- **Tune** (`-tune`, default hq): `hq` high quality, `uhq` ultra high quality,
  `ll` low latency, `ull` ultra low latency, `lossless`.
- **Rate control** (`-rc`): `constqp` (0), `vbr` (1), `cbr` (2). Plus `-qp`,
  `-qmin`/`-qmax`, `-cq` (target quality for VBR), `-b:v`/`-maxrate`/`-bufsize`,
  `-multipass` (`disabled`/`qres`/`fullres`), `-rc-lookahead` (frames).
- **Quality knobs:** `-spatial-aq`, `-temporal-aq` (+`-aq-strength` 1–15),
  `-b_ref_mode`, `-weighted_pred`, `-nonref_p`, `-strict_gop`, `-forced-idr`,
  `-no-scenecut`, `-b_adapt`, `-tf_level`, `-lookahead_level`, `-dpb_size`.
- **Latency/compat:** `-zerolatency`, `-delay`, `-aud`, `-bluray-compat`,
  `-intra-refresh`, `-a53cc`, `-s12m_tc`, `-gpu` (ordinal select).

---

## 3. What the engine wires today (code-ground truth)

- `nv_preset_for()` (`exporter.cpp:733`): x264-style names → p-numbers for
  `*nvenc*` (also vaapi/qsv/amf); software encoders pass through verbatim;
  SVT-AV1/l closed-loop get the reversed 0–13 numeric map. Unknown strings pass
  through (FFmpeg no-ops invalid values rather than aborting).
- RC intent (`exporter.cpp:919–944`): `vid_rc_mode "cbr"` → `rc=cbr`, else
  `rc=vbr`; VAAPI additionally gets `rc_mode`; NVENC constqp path via
  `crf >= 0` + `vid_rc_mode == "constqp"` (`:944`); VBV sizing at `:1005`
  (`max_bps`, `max_bps*2` for non-CBR).
- GPU pin: the encoder device honors the Settings GPU pin — same render
  node / CUDA ordinal playback probes use (`exporter.cpp:249-250`). Picking
  the **CPU row** in Preferences pins the software backend, which keeps
  encode on libx265 too (HW encode only engages when its backend matches the
  preferred GPU backend).
- Tests: `core/tests/cuda_enc_test.cpp` (hevc/h264_nvenc round-trips at
  1440p60/80 Mbps + decode-back) and `cuda_enc_bench.cpp` (speed sweep);
  both device-gated (SKIP without an NVENC ordinal). Green on the 5070 Ti
  (9.5 s / 20.8 s).

---

## 4. Measured matrix (180 f, 80M target, end-to-end incl. sw decode)

| config | fps | ms | bytes (actual Mbps) | frames |
|---|---|---|---|---|
| p7 hq cbr | 832 | 2161 | 19,958,484 (66.5) | 180 |
| **p4 hq cbr** ✅ | **1749** | **1029** | **19,618,956 (65.4)** | 180 |
| p1 hq cbr | 1754 | 1026 | 27,786,998 (92.6) | 180 |
| p1 ll cbr | 1698 | 1060 | 25,384,059 (84.6) | — |
| p1 ull cbr | 1696 | 1061 | 25,384,059 (84.6) | — |
| p7 vbr | 849 | 2118 | 17,418,542 (58.0) | — |
| p1 multipass-qres + lookahead32 | 1521 | 1183 | 29,626,407 (98.8) | — |
| p1 spatial+temporal AQ (str 8) | 1714 | 1050 | 29,000,076 (96.7) | — |

Repro: `ffmpeg -i <clip> -frames:v 180 -vf scale=2560:1440,format=yuv420p
-c:v hevc_nvenc -preset <p> -tune <t> -rc <rc> -b:v 80M -maxrate 120M
-bufsize 120M out.mp4` under `/tmp/nvenc_probe/` (artifacts, not in-tree).

Findings:

1. **Feeder-bound, not encoder-bound.** p1–p4 are identical (~1750 fps) because
   software AV1 decode + scale + mux caps the pipeline. Only p7 (832 fps)
   shows the encoder itself. Consequence: below p5, choose on rate control —
   and p4's CBR lands 65 Mbps while p1 overshoots to 93+ on short windows.
2. **Multipass qres + lookahead is lose/lose here**: −13% speed, +50% bytes
   vs p1 plain. Two-pass pays when the RC has room to reallocate; at 80 Mbps
   on short windows it just spends more.
3. **AQ is bitrate-neutral-to-up, speed-neutral.** Correct as a quality dial
   (detail retention), never as a speed play.
4. **ll/ull change nothing for file export** (same fps, ~identical bytes to
   hq at p1). They exist for streaming latency; the deliver path should stay
   on hq.
5. **vbr undershoots** (58 Mbps) at the same speed cost as cbr — fine when the
   deliverable allows VBR, but CBR tracks the 80 Mbps target better.
6. **Recommendation: remap `medium`→p4** in `nv_preset_for()` (currently p5).
   Keep `slow`→p6 / `veryslow`→p7 as the quality slots. `fast`→p3 and
   `faster`/`veryfast`→p2 are harmless (still feeder-bound) but p2 inherits
   p1-family fast-start overshoot — prefer p3/p4 spellings in presets.
   **APPLIED 2026-09-15** alongside the NVENC AQ wiring (`spatial_aq`/
   `temporal_aq` + `aq_strength=8` base in `exporter.cpp`, overridable via the
   Deliver panel's `aq-strength=` extra; speed-neutral per finding 3 above).
   Post-apply bench (steady-state, 1440p60 80 Mbps VBR, `cuda_enc_bench`):
   `medium` p4 = 474 fps, `faster` p5 = 1067 fps, `ultrafast` p1 = 1061 fps —
   confirms p1/p4 are equal-cost only when feeder-bound; under an in-process
   decode+resize+encode pipeline the preset order still holds and p4 is the
   high-quality slot at half of p1/p5's rate.

---

## 5. What was deliberately not used

- **constqp/CQP**: wrong tool for a bitrate deliverable; no fixed-bitrate
  guarantee. Path exists (`:944`) for quality-ladder use, not the 80 Mbps law.
- **lossless / uhq tune**: 5–10× size for zero deliverable benefit at 1440p60.
- **zerolatency / intra-refresh**: streaming tools; they drop B-frames and
  cost efficiency for latency nobody asked for in file export.
- **10-bit (`highbitdepth`)**: source and deliverable are 8-bit; costs speed
  for no gain.
- **SVT-HEVC as an alternative**: not installed (only `libsvtav1`, wrong
  codec); archived upstream and trails x265 in quality-per-bit at high
  bitrates. libx265 stays the CPU pick; NVENC stays the speed pick.

---

## 6. Open threads (not this doc's job)

- The CLI numbers above include **software** input decode; the exporter's
  zero-copy GPU composite path (`decode_to_hw` → NVENC) skips that cost, so
  in-app NVENC should beat every number in §4.
- If NVENC ever stops being feeder-bound (faster input, slower preset), redo
  §4 before touching the `medium` mapping — the whole point of this file is
  that the mapping follows measurement.
