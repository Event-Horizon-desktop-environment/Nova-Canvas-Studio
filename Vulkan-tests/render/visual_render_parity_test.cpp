// visual_render_parity_test — Vulkan identity render parity gate (Phase P-D).
//
// Phase 0 placeholder for the reading core's visual_render_test pins on the CPU
// path: an identity video render is byte-identical to the legacy fast path,
// and flip/scale/opacity change the output pixels. The Vulkan composite kernel
// (Phase P-D) is contractually built to the SAME law (see grade/gpu_grade_vulkan
// — the byte-exact host-mirror contract), and when it ships this test renders
// the same identity/transform scene through it and compares byte-for-byte.
//
// Until the P-D composite kernel exists, this reading cannot measure anything
// real, so it SKIPs with the owning phase named. It stays registered so the
// "reading never absent" exit gate holds from day one.
//
// PASS (0)  — never reached until P-D (would run the real parity render).
// FAIL (1)  — never reached until P-D.
// SKIP (2)  — composite kernel not resident yet; owning phase P-D.

#include <cstdio>
#include <cstdlib>

int main() {
    std::printf("SKIP  visual_render_parity: server-side Vulkan composite kernel "
                "not resident yet (owning phase P-D builds it to the grade-law "
                "contract; this reading then renders identity + flip/scale/opacity "
                "and compares byte-exact against gpu_grade_vulkan_test)\n");
    return 2;
}