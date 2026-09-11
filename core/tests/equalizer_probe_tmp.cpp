#include <cstdio>
#include <cmath>
#include <array>
#include "canvas/core/media/equalizer.hpp"
#include "canvas/core/timeline/model.hpp"
using namespace canvas::core;

static const int kRate = 48000;
static std::array<Clip::EqBand,6> one_band(Clip::EqBand::Type t, float f, float g, float q) {
    std::array<Clip::EqBand,6> bands{};
    for (auto& b : bands) { b.type = Clip::EqBand::Type::Bell; b.frequency=1000.f; b.gain=0.f; b.q=0.707f; b.enabled=true; }
    bands[0].type=t; bands[0].frequency=f; bands[0].gain=g; bands[0].q=q;
    return bands;
}

int main() {
    const int seg = 8000;
    const auto boost_12 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 12.0f, 1.0f);
    const auto boost_14 = one_band(Clip::EqBand::Type::Bell, 1000.0f, 14.0f, 1.0f);
    EqualizerBank bank;
    std::vector<float> carried(seg);
    for (int i=0;i<seg;++i) carried[i] = static_cast<float>(0.5*std::sin(2*M_PI*1000.0*i/kRate));
    (void)bank.tick(10, boost_12, true, kRate, 1, carried.data(), seg/2 - 64);
    (void)bank.tick(10, boost_14, true, kRate, 1, carried.data() + seg/2 - 64, seg/2 + 64);
    float max_step=0; int at=-1;
    for (int i = seg/2 - 65; i < seg/2 + 2; ++i) {
        float s = std::fabs(carried[i]-carried[i-1]);
        if (s>max_step){max_step=s;at=i;}
    }
    printf("max_step=%.4f at=%d (boundary=%d) around: ", max_step, at, seg/2-64);
    for (int i=seg/2-66;i<seg/2-61;++i) printf("%+.3f ", carried[i]);
    printf(" | ");
    for (int i=seg/2-64;i<seg/2-59;++i) printf("%+.3f ", carried[i]);
    // cold-start reference
    EqualizerBank bank2;
    auto cold = carried;
    (void)bank2.tick(10, boost_12, true, kRate, 1, cold.data(), seg/2 - 64);
    (void)bank2.tick(10, boost_12, true, kRate, 1, cold.data() + seg/2 - 64, 64); // keep same gains? no - want cold start of 14
    (void)bank2.drop(10);
    (void)bank2.tick(10, boost_14, true, kRate, 1, carried.data() + seg/2 - 64, seg/2 + 64);
    printf("\n--- with explicit cold (drop) ---\n");
    EqualizerBank bank3;
    auto coldrun = carried;
    (void)bank3.tick(10, boost_12, true, kRate, 1, coldrun.data(), seg/2 - 64);
    bank3.drop(10);
    (void)bank3.tick(10, boost_14, true, kRate, 1, coldrun.data() + seg/2 - 64, seg/2 + 64);
    float mx2=0;
    for (int i=seg/2-1; i<seg/2+2; ++i) mx2=std::max(mx2,std::fabs(coldrun[i]-coldrun[i-1]));
    printf("cold-start step at boundary=%.4f carry step=%.4f\n", mx2, max_step);
    return 0;
}
