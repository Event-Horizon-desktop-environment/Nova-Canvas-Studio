#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core {

struct AudioWaveform {
    std::size_t buckets = 0;
    std::vector<float> peak;  // per-bucket peak, normalized 0..1
    std::vector<float> rms;   // per-bucket rms, normalized 0..1
    double duration_seconds = 0.0;
};

// Decodes the audio stream of `path` and computes per-bucket peak/RMS
// amplitude levels for a simple timeline waveform preview.
bool decode_audio_waveform(const std::string& path, std::size_t buckets,
                           AudioWaveform* out, std::string* error = nullptr);

// Re-samples a higher-resolution waveform into `out_buckets` buckets so the
// caller can decode the full audio file once (at a fixed high resolution) and
// cheaply produce a preview at any pixel width. `src` is the high-resolution
// source; `out` receives peak/rms arrays of length `out_buckets`. No file I/O.
AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets);

// Range-aware variant: only the source fraction `[lo_frac, hi_frac)` (both in
// 0..1, measuring position within `src`) is redistributed into `out_buckets`,
// so a clip that displays just its src_in..src_out window shows exactly that
// audio instead of the whole file squashed into its width. This is what keeps
// the waveform identical across a blade cut — each half re-renders the same
// source samples it covered before the split. Falls back to the whole file
// when `lo_frac >= hi_frac` or the range is outside [0,1].
AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets,
                              double lo_frac, double hi_frac);

}  // namespace canvas::core
