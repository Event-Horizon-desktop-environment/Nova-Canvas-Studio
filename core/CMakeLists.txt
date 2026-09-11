find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

find_package(Threads REQUIRED)

# Optional CUDA acceleration (GPU RGBA->NV12 conversion to feed NVENC directly).
# Works with CUDA 12 and 13. If no nvcc is found the GPU path is simply
# disabled and the project falls back to CPU swscale — so the build still
# succeeds on non-NVIDIA machines.
#
# Prefer CMake's native CUDAToolkit discovery, which searches the standard
# install locations (/usr/local/cuda*, versioned dirs, CUDACXX, ...) rather than
# relying on nvcc sitting on $PATH. This matters because elevated builds
# (`sudo just install`, package builds) often run with a stripped PATH that omits
# /usr/local/cuda/bin even though the toolkit is installed.
find_package(CUDAToolkit QUIET)
set(CANVAS_CUDA_ENABLED OFF)
if(CUDAToolkit_FOUND)
    set(CANVAS_NVCC "${CUDAToolkit_NVCC_EXECUTABLE}")
    set(CMAKE_CUDA_COMPILER "${CANVAS_NVCC}" CACHE FILEPATH "nvcc" FORCE)
    # CUDA 12/13 rejects host compilers newer than its supported max (e.g.
    # CUDA 13 on Debian's GCC 16). The flag is accepted (as a no-op warning) on
    # all supported versions, so always pass it to stay portable.
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler")
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    enable_language(CUDA)
    set(CANVAS_CUDA_ENABLED ON)
    else()
        # Fallback: locate nvcc by hand if CUDAToolkit discovery didn't (e.g. a
        # non-standard prefix). Respect an explicit CUDA_HOME/CUDACXX, and probe
        # the common non-PATH install prefixes (/opt/cuda, /usr/local/cuda, ...)
        # so distros that put the toolkit outside PATH still get the GPU path.
        find_program(CANVAS_NVCC nvcc
            HINTS "${CUDA_HOME}/bin" "${CUDACXX}" ENV CUDACXX
                  /opt/cuda/bin /usr/local/cuda/bin /usr/cuda/bin
            PATH_SUFFIXES bin
            NO_DEFAULT_PATH
        )
        if(NOT CANVAS_NVCC)
            find_program(CANVAS_NVCC nvcc)
        endif()
        if(CANVAS_NVCC)
            set(CMAKE_CUDA_COMPILER "${CANVAS_NVCC}" CACHE FILEPATH "nvcc" FORCE)
            set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler")
            set(CMAKE_CUDA_STANDARD 17)
            set(CMAKE_CUDA_STANDARD_REQUIRED ON)
            enable_language(CUDA)
            set(CANVAS_CUDA_ENABLED ON)
            # find_package(CUDAToolkit) failed, so the imported CUDA::cudart
            # target does not exist. Derive the runtime library from the same
            # prefix as nvcc instead and link it by path below.
            set(CANVAS_CUDA_NVCC_BIN "${CMAKE_CUDA_COMPILER}")
            get_filename_component(CANVAS_CUDA_ROOT "${CANVAS_CUDA_NVCC_BIN}" DIRECTORY)
            get_filename_component(CANVAS_CUDA_ROOT "${CANVAS_CUDA_ROOT}" DIRECTORY)
            find_library(CANVAS_CUDA_CUDART NAMES cudart
                HINTS "${CANVAS_CUDA_ROOT}/lib64" "${CANVAS_CUDA_ROOT}/lib"
                      "${CANVAS_CUDA_ROOT}/targets/x86_64-linux/lib"
                NO_DEFAULT_PATH)
            set(CANVAS_CUDA_RUNTIME "${CANVAS_CUDA_CUDART}")
            # find_package(CUDAToolkit) also normally supplies cuda_runtime.h's
            # directory via CUDA::cudart's INTERFACE_INCLUDE_DIRECTORIES. Since
            # that imported target doesn't exist on this path, locate the header
            # by hand from the same prefix as nvcc, or any C++ (non-.cu)
            # translation unit that includes <cuda_runtime.h> directly (e.g.
            # cuda_convert.hpp, vram_leak_test.cpp) fails with "No such file".
            find_path(CANVAS_CUDA_INCLUDE_DIR cuda_runtime.h
                HINTS "${CANVAS_CUDA_ROOT}/include"
                      "${CANVAS_CUDA_ROOT}/targets/x86_64-linux/include"
                NO_DEFAULT_PATH)
        endif()
    endif()

    # The imported target is only valid when find_package(CUDAToolkit) succeeded.
    if(CANVAS_CUDA_ENABLED AND NOT CANVAS_CUDA_RUNTIME)
        set(CANVAS_CUDA_RUNTIME CUDA::cudart)
    endif()

message(STATUS "Nova Canvas CUDA acceleration: ${CANVAS_CUDA_ENABLED} (nvcc=${CANVAS_NVCC})")

add_library(canvas_core STATIC
    src/media/frame_cache.cpp
    src/media/video_decoder.cpp
    src/media/audio_decoder.cpp
    src/media/hw_device.cpp
    src/media/audio_waveform.cpp
    src/timeline/model.cpp
    src/timeline/edit_ops.cpp
    src/project/project.cpp
    src/export/renderer.cpp
    src/export/exporter.cpp
    src/export/deliver_preset.cpp
    src/export/render_queue.cpp
)
# canvas_core is deliberately Qt-free; the root CMakeLists enables AUTOMOC globally,
# which would otherwise try (and fail) to run moc over these files. Disable it
# explicitly so configure is quiet and there's no spurious AUTOGEN warning.
set_target_properties(canvas_core PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(canvas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(canvas_core PUBLIC
    PkgConfig::LIBAV
    Threads::Threads
)

if(CANVAS_CUDA_ENABLED)
    target_sources(canvas_core PRIVATE src/gpu/cuda_convert.cu)
    target_compile_definitions(canvas_core PUBLIC CANVAS_HAVE_CUDA=1)
    target_link_libraries(canvas_core PUBLIC "${CANVAS_CUDA_RUNTIME}")
    if(CANVAS_CUDA_INCLUDE_DIR)
        target_include_directories(canvas_core PUBLIC "${CANVAS_CUDA_INCLUDE_DIR}")
    endif()
endif()

add_executable(canvas_roundtrip_test tests/roundtrip_test.cpp)
target_link_libraries(canvas_roundtrip_test PRIVATE canvas_core)
set_target_properties(canvas_roundtrip_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME roundtrip COMMAND canvas_roundtrip_test)

add_executable(canvas_export_sweep_test tests/export_sweep_test.cpp)
target_link_libraries(canvas_export_sweep_test PRIVATE canvas_core)
set_target_properties(canvas_export_sweep_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME export_sweep COMMAND canvas_export_sweep_test)

add_executable(canvas_scrub_bench_test tests/scrub_bench_test.cpp)
target_link_libraries(canvas_scrub_bench_test PRIVATE canvas_core)
set_target_properties(canvas_scrub_bench_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME scrub_bench COMMAND canvas_scrub_bench_test)

add_executable(canvas_visual_render_test tests/visual_render_test.cpp)
target_link_libraries(canvas_visual_render_test PRIVATE canvas_core)
set_target_properties(canvas_visual_render_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME visual_render COMMAND canvas_visual_render_test)
