# Vulkan-tests — Phase 0 of the Vulkan backend plan (see docs/vulkan.md Part V).
#
# Test-first: the backend is proven by this suite BEFORE any implementation
# (Phases P-A .. P-G land the hardware paths underneath these gates). Special
# exit-code convention on every test here:
#   0 = PASS   1 = FAIL   2 = SKIP (capability absent; probe names the phase)
#
# Headless by construction: links only canvas_core + Vulkan, never Qt. check_qtdep
# enforces that statically (Vulkan-tests is allowlisted in scripts/check_qtdep.sh).

get_filename_component(CANVAS_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" ABSOLUTE)

# The shared capability probe library (instance/device/queue probing + driver
# matrix report). All tests in this tree build on it.
add_library(canvas_vk_probe STATIC
    common/vk_probe.hpp
    common/vk_probe.cpp
)
target_include_directories(canvas_vk_probe PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CANVAS_CORE_SRC}
)
target_link_libraries(canvas_vk_probe PUBLIC
    Vulkan::Vulkan
    canvas_core
)
set_target_properties(canvas_vk_probe PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

# Capability/session helpers shared by the device-level readings.
add_library(canvas_vk_common STATIC
    common/vk_session.hpp
    common/vk_session.cpp
)
target_include_directories(canvas_vk_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CANVAS_CORE_SRC}
)
target_link_libraries(canvas_vk_common PUBLIC
    Vulkan::Vulkan
    canvas_vk_probe
)
set_target_properties(canvas_vk_common PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

# Build a headless Qt-free Vulkan test executable (test source + probe lib).
function(canvas_add_vulkan_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CANVAS_CORE_SRC}
    )
    target_link_libraries(${name} PRIVATE
        canvas_vk_common
        Vulkan::Vulkan
        canvas_core
    )
    set_target_properties(${name} PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# 1. The probe itself, as a selftest: instance/device/queue-family/video-cap
#    report must come back coherent (loader present, device present, queue
#    matrix sane). SKIP (2) only when there is NO Vulkan loader/ICD at all.
canvas_add_vulkan_test(vk_probe_test
    probe/vk_probe_test.cpp
)

# 2. gpu_grade_vulkan: host-mirror law pin for the fused composite kernel's
#    grade (the nv12GradeResize host-mirror parity, mirroring gpu_grade_test).
#    This reading exists so P-D's Vulkan composite kernel is proven against the
#    SAME law before it ships. Runs headless, no device needed.
canvas_add_vulkan_test(gpu_grade_vulkan_test
    grade/gpu_grade_vulkan_test.cpp
)

# 3. Driver-matrix capability readings. Each answers a question the backend
#    phases must answer before building on this machine.
canvas_add_vulkan_test(queue_matrix_test
    caps/queue_matrix_test.cpp
)
canvas_add_vulkan_test(video_profiles_test
    caps/video_profiles_test.cpp
)
canvas_add_vulkan_test(formats_test
    caps/formats_test.cpp
)
canvas_add_vulkan_test(interop_contract_test
    caps/interop_contract_test.cpp
)

# 4. vram_leak_vulkan: device-memory steady-state gate using VK_EXT_memory_budget
#    (the GPU-side sibling of core vram_leak). SKIP (2) when the extension or a
#    Vulkan decode path is absent — the probe names the owning phase.
canvas_add_vulkan_test(vram_leak_vulkan_test
    vram/vram_leak_vulkan_test.cpp
)

# 5. export_sweep_vulkan: encoder sweep over the Vulkan hw encoders FFmpeg
#    exposes (h264/hevc/av1_prores/prores_ks) when present. SKIP (2) when the
#    local FFmpeg has none compiled in.
canvas_add_vulkan_test(export_sweep_vulkan_test
    encode/export_sweep_vulkan_test.cpp
)

# 6. Phase-owning SKIP stubs: the "reading never absent" gate. Each registers a
#    target that reports the owning phase and exits 2 until that phase lands.
canvas_add_vulkan_test(scrub_bench_vulkan_test
    scrub/scrub_bench_vulkan_test.cpp
)
canvas_add_vulkan_test(visual_render_parity_test
    render/visual_render_parity_test.cpp
)