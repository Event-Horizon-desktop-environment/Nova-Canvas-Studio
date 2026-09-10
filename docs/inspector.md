# Inspector Tabs Implementation Plan

**Goal:** Build out the Audio, Transition, and File inspector tabs with full model backing, undoable edit ops, and wired UI. Audio tab only active for audio clips; Transition tab only active when a transition bubble is selected; File tab fully wired for all clips.

---

## Phase 1: Core Model + edit_ops

### 1.1 New Clip fields (`core/include/canvas/core/timeline/model.hpp`)

```cpp
// Pitch shift
float pitch_semitones = 0.0f;   // -12 .. +12
float pitch_cents = 0.0f;       // -100 .. +100

// Speed change
float speed_factor = 1.0f;      // 0.1 .. 10.0
bool speed_enabled = false;

// Parametric equalizer (6 bands)
struct EqBand {
    enum class Type { LowShelf, Bell, HighShelf, LowPass, HighPass, Notch };
    Type type = Type::Bell;
    float frequency = 1000.0f;
    float gain = 0.0f;
    float q = 1.0f;
};
bool eq_enabled = false;
std::array<EqBand, 6> eq_bands;

// Clip metadata
enum class ClipTag : uint8_t { None = 0, GoodTake, Rejected };
ClipTag clip_tag = ClipTag::None;
uint8_t clip_color = 0;         // 0=none, 1-12 = swatch index
std::string comments;
```

### 1.2 New edit_ops (`edit_ops.hpp` / `edit_ops.cpp`)

```cpp
set_clip_audio_processing(seq, kind, track_idx, clip_id,
    pitch_semitones, pitch_cents, speed_factor, speed_enabled,
    eq_enabled, eq_bands) → ICommand

set_clip_transition_curve(seq, kind, track_idx, clip_id,
    in_edge, ease_amount, curve_value) → ICommand

set_clip_metadata(seq, kind, track_idx, clip_id,
    tag, color, comments, name) → ICommand
```

All follow existing snapshot-based undo pattern. Linked mates inherit values.

---

## Phase 2: InspectorAudio module

**Files:** `gui/src/UX/InspectorAudio.hpp` + `InspectorAudio.cpp`

### Categories:

| # | Category | Controls | Wired |
|---|----------|----------|-------|
| 1 | Audio (Volume/Pan) | Volume dB spin (-60..+24), Pan spin (-1..+1) | Yes (moved from ShellDocks) |
| 2 | Pitch | Semi Tones spin+slider (-12..+12), Cents spin+slider (-100..+100) | Yes |
| 3 | Speed Change | Enable toggle, Speed factor spin+slider (0.1..10.0) | Yes |
| 4 | Equalizer | Enable toggle, EQ graph widget, 6 bands × (type/freq/gain/Q) | Yes |
| 5 | AI Voice Isolation | Enable toggle (placeholder), Amount slider | No (UI only) |
| 6 | AI Dialogue Leveler | Enable toggle (placeholder) | No (UI only) |
| 7 | AI Music Remixer | Enable toggle (placeholder) | No (UI only) |

### EQ Band defaults:

| Band | Type | Freq | Gain | Q |
|------|------|------|------|---|
| B1 | LowShelf | 20 Hz | +18.1 dB | — |
| B2 | Bell | 57 Hz | +18.1 dB | — |
| B3 | Bell | 97 Hz | +10.5 dB | 1.0 |
| B4 | Bell | 1.2K Hz | 0.0 dB | 1.0 |
| B5 | HighShelf | 6.0K Hz | 0.0 dB | — |
| B6 | LowPass | 19.0K Hz | — | — |

### Key behavior:
- Tab **only enabled** when audio clip selected (`clip->kind == Track::Kind::Audio`)
- Video clip selected → tab greys out or shows only Volume/Pan
- All changes via `set_clip_audio_processing` → undo → snapshot push

---

## Phase 3: InspectorTransition module

**Files:** `gui/src/UX/InspectorTransition.hpp` + `InspectorTransition.cpp`

### Structure:

Top: **Start / End** sub-tab pill row (exclusive QButtonGroup)

**Start view (default):**
- **Video** category:
  - Transition Type: QComboBox (Cross Dissolve, DipToBlack, FadeOut, FadeIn, Wipe*)
  - Duration: seconds + frames display, editable
  - "Set as Default Duration" button
  - Alignment: 3-button group (left/center/right)
  - Style: QComboBox (Video)
  - Start/End Ratio: slider + spin (0..100)
  - Ease: QComboBox (None, Ease In, Ease Out, Ease In-Out)
  - Transition Curve: slider + spin (0.000..1.000), keyframe nav, reset

- **Audio** category:
  - Transition Type: QComboBox (Cross Fade 0/3/6 dB)
  - Duration: seconds + frames
  - "Set as Default Duration" button (disabled)
  - Alignment: 3-button group
  - Fade In/Out: QComboBox (0/3/6 dB)

**End view:** Same structure, different defaults (center-aligned, curve=0.000)

### Key behavior:
- Tab **only enabled** when `timeline_->has_selected_transition()` is true
- New signal: `transition_selected_for_inspector` emitted on bubble click
- Duration changes → `transition_resized`/`transition_in_resized` (reuse existing)
- Type changes → `set_clip_transition`/`set_clip_transition_in` (reuse existing)
- Curve/ease → `set_clip_transition_curve` (new)

---

## Phase 4: InspectorFile module

**Files:** `gui/src/UX/InspectorFile.hpp` + `InspectorFile.cpp`

### Categories:

| # | Category | Controls | Wired |
|---|----------|----------|-------|
| 1 | Header Info | Read-only labels (filename, duration, codecs, fps, resolution, sample rate) | Read-only |
| 2 | Metadata | Timecode, tag (3-button), color (swatch row), name, comments | Yes |
| 3 | Audio Configuration | Format combo, channel rows, play/stop, level | Partial |
| 4 | Timecode | Current frame, slate, offset | Read-only |

### Editable fields wired via `set_clip_metadata`:
- Tag: Good Take / Untagged / Rejected toggle
- Color: 12-swatch row + clear button
- Name: QLineEdit
- Comments: QTextEdit

### Key behavior:
- Header info probed from `VideoDecoder::open()` (cached per media)
- All editable changes produce undoable edits

---

## Phase 5: MainWindow + ShellDocks + CMakeLists wiring

### MainWindow.hpp additions:
- Friend declarations for all 3 new modules (build/attach/update/apply)
- Member variables: `transition_inspector_active_`, `transition_inspector_kind_`, `transition_inspector_track_`, `transition_inspector_clip_`, `transition_inspector_in_edge_`

### ShellDocks.cpp changes:
- Replace placeholder loop with calls to `build_inspector_transition()` and `build_inspector_file()`
- Audio page delegates to `build_inspector_audio()` for new categories

### TimelineActions.cpp additions:
- Connect `transition_selected_for_inspector` signal
- Call `update_inspector_audio_full()` on clip selection
- Call `update_inspector_transition()` on transition selection
- Call `update_inspector_file()` on clip selection

### CMakeLists.txt:
Add `InspectorAudio.cpp`, `InspectorTransition.cpp`, `InspectorFile.cpp`

---

## Phase 6: Build + test verification

- `./build.sh` — zero warnings
- `ctest --test-dir build` — existing tests pass
- Manual: audio clip → Audio tab enables, pitch/speed/EQ controls work
- Manual: transition bubble → Transition tab enables, Start/End sub-tabs work
- Manual: any clip → File tab shows metadata, editable fields commit with undo

---

## File Change Summary

| File | Action | Phase |
|------|--------|-------|
| `core/.../model.hpp` | Add Clip fields | 1 |
| `core/.../edit_ops.hpp` | Declare 3 new ops | 1 |
| `core/src/.../edit_ops.cpp` | Implement 3 new ops | 1 |
| `gui/src/UX/InspectorAudio.hpp` | NEW | 2 |
| `gui/src/UX/InspectorAudio.cpp` | NEW (~400 lines) | 2 |
| `gui/src/UX/InspectorTransition.hpp` | NEW | 3 |
| `gui/src/UX/InspectorTransition.cpp` | NEW (~350 lines) | 3 |
| `gui/src/UX/InspectorFile.hpp` | NEW | 4 |
| `gui/src/UX/InspectorFile.cpp` | NEW (~300 lines) | 4 |
| `gui/src/UX/MainWindow.hpp` | Add friends + members | 5 |
| `gui/src/UX/ShellDocks.cpp` | Replace placeholders | 5 |
| `gui/src/features/timeline/TimelineActions.cpp` | Add signal handlers | 5 |
| `gui/CMakeLists.txt` | Add 3 new .cpp | 5 |

---

## Status: all phases done + verified (2026-09-06)

Phases 1–6 are complete. Verified on this machine: Release and Debug builds are
zero-warning, `ctest --test-dir build` passes **13/13** (incl. the new
`visual_render_test`, which also exposed + fixed a half-pixel sampling bug in
`blit_rgba_transformed`: it now samples at pixel centres so pure
flips/identity are byte-exact), and `./scripts/check_qtdep.sh` passes.

Deviation notes vs. the plan above (all deliberate, documented in code):

- **Audio tab gating:** the Audio page is enabled only when an audio-kind clip
  is selected (per-user decision); a video clip selection greys the whole tab.
  Waveform transparency/reflect is NOT implemented (deferred). AI sections are
  UI-only placeholders. EQ Q control is visible only for Bell/HighPass/Notch.
- **Transition tab:** uses `TimelineWidget::transition_selected` +
  `transition_selection_cleared` (new signals emitted from
  `select_transition_bubble` / `clear_selected_transition`, with
  `selected_transition_a()/b()/in_edge()` accessors) instead of the planned
  `transition_selected_for_inspector`. Shaping is now genuinely PER EDGE:
  `set_clip_transition_curve` writes `transition_{out,in}_{curve_value,ease,
  start_ratio,end_ratio}` with the reference defaults (Start/OUT curve 1.000,
  End/IN curve 0.000, ratios 0/100 spanning the window). Start/End ratio are
  independent editable fields, alignment defaults per side (Start right, End
  center, UI-state only), and the Transition pill itself is disabled unless a
  bubble is selected. Audio category edits the linked mate's IN/OUT fade.
  "Set as Default Duration" stores a UI-side default per edge only.
- **File tab:** Header Info reads the `MediaEntry` (streams auto-detected from
  the clip kind / linked audio mate) rather than `VideoDecoder::open`; the
  per-channel play/stop buttons are disabled in this build ("not connected").
  Timecode editing relocates the clip via `move_clip`.
- **Existing-test win:** adding `EqBand::operator==` (required by the audio
  inspector's dirty check) also fixed the long-standing `roundtrip` SEGFAULT
  (stale-timeline crash), so all 13 tests now pass.
