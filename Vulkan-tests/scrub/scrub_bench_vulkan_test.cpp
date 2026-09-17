extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdio>
#include <cstdlib>

int main() {
    const AVCodec* dec = avcodec_find_decoder_by_name("h264_vulkan");
    if (dec == nullptr) {
        std::printf("SKIP  scrub_bench_vulkan: linked FFmpeg has no h264_vulkan decoder "
                    "(owning phase P-C installs the self-built FFmpeg + decode path)\n");
        return 2;
    }
    std::printf("note: scrub_bench_vulkan found h264_vulkan decoder; P-C gate not "
                "implemented yet\n");
    return 2;
}