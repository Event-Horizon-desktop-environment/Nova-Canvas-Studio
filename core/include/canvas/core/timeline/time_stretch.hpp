#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace canvas::core {

class TimeStretch {
public:
    TimeStretch() = default;
    ~TimeStretch() = default;
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;
    TimeStretch(TimeStretch&&) noexcept = default;
    TimeStretch& operator=(TimeStretch&&) noexcept = default;

    [[nodiscard]] static int lookahead_frames(int sample_rate) noexcept;

    [[nodiscard]] static int src_margin_frames() noexcept;

    [[nodiscard]] static int retime_lookahead(int sample_rate, double speed_ratio,
                                              double pitch_factor) noexcept;

    [[nodiscard]] int process(const float* in, int in_frames, int channels,
                              double speed_ratio, double pitch_factor, float pan,
                              int out_frames, int sample_rate,
                              std::vector<float>& out);

    [[nodiscard]] int out_channels() const noexcept { return out_channels_; }

    void reset();

private:
    static constexpr int kSrcTapsHalf = 16;
    static double sinc(double x) noexcept;

    void setup(int sample_rate, int channels);
    void compact_history();
    void compact_media();
    void emit_src(int64_t media_end);
    int search_best_offset(int64_t candidate) const;
    int run_synth(int out_frames);
    int run_passthrough(int out_frames);
    void finalize(std::vector<float>& out, int written, float pan);

    std::vector<float> window_;
    std::vector<float> in_hist_;
    std::vector<float> media_hist_;
    std::vector<float> stage_;
    std::vector<float> cur_win_;
    std::vector<float> prev_tail_;
    std::vector<float> prev_sig_;

    int sample_rate_ = 0;
    int channels_ = 0;
    int out_channels_ = 0;
    int W_ = 0;
    int S_ = 0;
    int D_ = 0;

    int64_t in_base_ = 0;
    int64_t media_base_ = 0;
    double src_pos_ = 0.0;
    double pitch_ = 1.0;
    double wsola_ratio_ = 1.0;
    int64_t cur_s_ = 0;
    int64_t s_ = 0;
    int64_t out_base_ = 0;
    double a_ = 0.0;
    bool first_ = true;
};

class TimeStretchBank {
public:
    [[nodiscard]] int tick(std::uint64_t clip_id, double speed_ratio,
                           double pitch_factor, float pan, int sample_rate,
                           int channels, const float* in, int in_frames,
                           int out_frames, std::vector<float>& out,
                           int* out_channels = nullptr);

    void drop();
    void drop(std::uint64_t clip_id);
    void clear();

private:
    struct Entry {
        TimeStretch engine;
        double ratio = 0.0;
        double pitch = 1.0;
    };
    std::map<std::uint64_t, Entry> entries_;
};

}
