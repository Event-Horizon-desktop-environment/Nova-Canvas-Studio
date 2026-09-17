get_filename_component(CANVAS_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" ABSOLUTE)

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

canvas_add_vulkan_test(vk_probe_test
    probe/vk_probe_test.cpp
)

canvas_add_vulkan_test(gpu_grade_vulkan_test
    grade/gpu_grade_vulkan_test.cpp
)

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

canvas_add_vulkan_test(vram_leak_vulkan_test
    vram/vram_leak_vulkan_test.cpp
)

canvas_add_vulkan_test(export_sweep_vulkan_test
    encode/export_sweep_vulkan_test.cpp
)

canvas_add_vulkan_test(scrub_bench_vulkan_test
    scrub/scrub_bench_vulkan_test.cpp
)
canvas_add_vulkan_test(visual_render_parity_test
    render/visual_render_parity_test.cpp
)