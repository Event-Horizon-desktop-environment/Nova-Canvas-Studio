#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace canvas::core {

struct AudioWaveform {
    std::size_t buckets = 0;
    std::vector<float> peak;
    std::vector<float> rms;
    double duration_seconds = 0.0;
};

bool decode_audio_waveform(const std::string& path, std::size_t buckets,
                           AudioWaveform* out, std::string* error = nullptr);

AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets);

AudioWaveform reduce_waveform(const AudioWaveform& src, std::size_t out_buckets,
                              double lo_frac, double hi_frac);

}
