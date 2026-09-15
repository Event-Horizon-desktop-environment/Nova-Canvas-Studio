find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

# Optional direct libva use: the zero-copy VAAPI playback path exports decoded
# NV12 surfaces via vaExportSurfaceHandle() and reads the driver identity via
# vaQueryDriverName(). FFmpeg's avcodec already load libva, but OUR *direct*
# calls need a real link — add it when libva dev headers/pc are present. The
# vendor/registry modules (amd/intel/nvidia) and the surface type always
# compile; the descriptor export itself is gated on CANVAS_HAVE_VAAPI so a
# libva-less build falls back to the CPU download path (as it does today).
pkg_check_modules(LIBVA IMPORTED_TARGET libva)
set(CANVAS_VAAPI_ENABLED OFF)
if(LIBVA_FOUND)
    set(CANVAS_VAAPI_ENABLED ON)
endif()
message(STATUS "Nova Canvas VAAPI zero-copy: ${CANVAS_VAAPI_ENABLED}")

# Vendored xiph/rnnoise (BSD-3-Clause) — the real-time speech noise-suppression
# engine behind Clip::voice_isolation = RnNoise. Self-contained: the model
# weights are compiled in via src/rnn_data.c, so no external .rnnn file ships.
# `-w` suppresses warnings for this third-party C library (it is exempt from the
# repo's zero-warning discipline); `-D_GNU_SOURCE` is the feature-test macro the
# upstream autotools build provides via AC_USE_SYSTEM_EXTENSIONS (exposes M_PI).
enable_language(C)
add_library(rnnoise STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/denoise.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/rnn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/rnn_data.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/kiss_fft.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/pitch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/celt_lpc.c
)
target_include_directories(rnnoise PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src
)
target_compile_options(rnnoise PRIVATE -w)
target_compile_definitions(rnnoise PRIVATE _GNU_SOURCE=1)
set_target_properties(rnnoise PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

find_package(Threads REQUIRED)

# Local voice-transcription engine (the Phase A headless transcribe job).
# FetchedContent, pinned at a tag, built with everything upstream off so only
# the static `whisper` library target (ggml + whisper.cpp) is compiled. The
# engine is consumed behind canvas::core::transcribe, which forces the CPU
# backend — the CUDA cost of a BACKGROUND transcription job is not worth it,
# and it keeps the module free of a CUDA-runtime dependency. Third-party code
# is exempt from the repo's zero-warning discipline (same as rnnoise).
include(FetchContent)
# The fetched whisper project declares cmake_minimum_required below the 3.10
# policy floor; claiming our own floor here silences its per-configure
# "Compatibility with CMake < 3.10" deprecation without touching upstream.
# (Raising the effective floor also snaps whisper/ggml onto the same policy set
# as the rest of the tree, which is benign under the WHISPER_* overrides.)
set(CMAKE_POLICY_VERSION_MINIMUM 3.24)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(WHISPER_OPENMP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(whispercpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.9.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(whispercpp)
message(STATUS "Nova Canvas whisper.cpp engine: pinned v1.9.4 (transcribe seam)")
# The root CMakeLists sets CMAKE_AUTOMOC ON globally; whisper/ggml inherited it
# and warn "No valid Qt version found". Silence autogen on the fetched targets
# (parakeet is the TransformerTalker backend added upstream in 1.9.x). The
# faint -Wformat-truncation inside whisper.cpp is third-party and only provable
# in optimized builds, so it is suppressed at the target scope rather than
# patched in _deps (the repo's third-party zero-warning exemption).
foreach(_w_t ggml ggml-cpu ggml-base whisper parakeet)
    if(TARGET ${_w_t})
        set_target_properties(${_w_t} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(_w_t ggml ggml-cpu ggml-base whisper parakeet)
        if(TARGET ${_w_t})
            get_target_property(_w_type ${_w_t} TYPE)
            if(NOT _w_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_w_t} PRIVATE -Wno-format-truncation)
            endif()
        endif()
    endforeach()
endif()

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
    src/media/sw_decode.cpp
    src/media/audio_decoder.cpp
    src/media/hw_device.cpp
    src/media/gpu_select.cpp
    src/media/audio_waveform.cpp
    src/media/voice_isolation.cpp
    src/media/equalizer.cpp
    src/media/transcript.cpp
    src/media/transcribe.cpp
    src/timeline/model.cpp
    src/timeline/edit_ops.cpp
    src/timeline/time_stretch.cpp
    src/timeline/title.cpp
    src/timeline/captions.cpp
    src/colorsci/wheels_ui.cpp
    src/colorsci/curves.cpp
    src/colorsci/histogram.cpp
    src/grade_graph/graph.cpp
    src/grade_graph/eval.cpp
    src/grade_graph/composite.cpp
    src/grade_graph/op.cpp
    src/grade_graph/edit.cpp
    src/grade_graph/serialize.cpp
    src/grade_graph/lut.cpp
    src/project/project.cpp
    src/export/renderer.cpp
    src/export/grade_frame.cpp
    src/export/exporter.cpp
    src/export/deliver_preset.cpp
    src/export/vaapi_encode.cpp
    src/export/qsv_encode.cpp
    src/export/render_queue.cpp

    src/media/vaapi/surface.cpp
    src/media/vaapi/driver.cpp
    src/media/vaapi/amd.cpp
    src/media/vaapi/intel.cpp
    src/media/vaapi/nvidia.cpp
    src/media/vaapi/export.cpp
)
# canvas_core is deliberately Qt-free; the root CMakeLists enables AUTOMOC globally,
# which would otherwise try (and fail) to run moc over these files. Disable it
# explicitly so configure is quiet and there's no spurious AUTOGEN warning.
set_target_properties(canvas_core PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(canvas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(canvas_core PUBLIC
    PkgConfig::LIBAV
    Threads::Threads
    rnnoise
    whisper
)

if(CANVAS_CUDA_ENABLED)
    target_sources(canvas_core PRIVATE src/gpu/cuda_convert.cu)
    target_compile_definitions(canvas_core PUBLIC CANVAS_HAVE_CUDA=1)
    target_link_libraries(canvas_core PUBLIC "${CANVAS_CUDA_RUNTIME}")
    if(CANVAS_CUDA_INCLUDE_DIR)
        target_include_directories(canvas_core PUBLIC "${CANVAS_CUDA_INCLUDE_DIR}")
    endif()
endif()

if(CANVAS_VAAPI_ENABLED)
    target_compile_definitions(canvas_core PUBLIC CANVAS_HAVE_VAAPI=1)
    target_link_libraries(canvas_core PUBLIC PkgConfig::LIBVA)
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

add_executable(canvas_colorsci_test tests/colorsci_test.cpp)
target_link_libraries(canvas_colorsci_test PRIVATE canvas_core)
set_target_properties(canvas_colorsci_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME colorsci COMMAND canvas_colorsci_test)

add_executable(canvas_graph_test tests/graph_test.cpp)
target_link_libraries(canvas_graph_test PRIVATE canvas_core)
set_target_properties(canvas_graph_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME graph COMMAND canvas_graph_test)

add_executable(canvas_graph_edit_test tests/graph_edit_test.cpp)
target_link_libraries(canvas_graph_edit_test PRIVATE canvas_core)
set_target_properties(canvas_graph_edit_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME graph_edit COMMAND canvas_graph_edit_test)

add_executable(canvas_composite_test tests/composite_test.cpp)
target_link_libraries(canvas_composite_test PRIVATE canvas_core)
set_target_properties(canvas_composite_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME composite COMMAND canvas_composite_test)

add_executable(canvas_op_test tests/op_test.cpp)
target_link_libraries(canvas_op_test PRIVATE canvas_core)
set_target_properties(canvas_op_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME op COMMAND canvas_op_test)

add_executable(canvas_lut_test tests/lut_test.cpp)
target_link_libraries(canvas_lut_test PRIVATE canvas_core)
set_target_properties(canvas_lut_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME lut COMMAND canvas_lut_test)

add_executable(canvas_gpu_grade_test tests/gpu_grade_test.cpp)
target_link_libraries(canvas_gpu_grade_test PRIVATE canvas_core)
set_target_properties(canvas_gpu_grade_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME gpu_grade COMMAND canvas_gpu_grade_test)

add_executable(canvas_wheels_ui_test tests/wheels_ui_test.cpp)
target_link_libraries(canvas_wheels_ui_test PRIVATE canvas_core)
set_target_properties(canvas_wheels_ui_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME wheels_ui COMMAND canvas_wheels_ui_test)

add_executable(canvas_curves_test tests/curves_test.cpp)
target_link_libraries(canvas_curves_test PRIVATE canvas_core)
set_target_properties(canvas_curves_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME curves COMMAND canvas_curves_test)

add_executable(canvas_histogram_test tests/histogram_test.cpp)
target_link_libraries(canvas_histogram_test PRIVATE canvas_core)
set_target_properties(canvas_histogram_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME histogram COMMAND canvas_histogram_test)

add_executable(canvas_visual_render_test tests/visual_render_test.cpp)
target_link_libraries(canvas_visual_render_test PRIVATE canvas_core)
set_target_properties(canvas_visual_render_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME visual_render COMMAND canvas_visual_render_test)

add_executable(canvas_voice_isolation_test tests/voice_isolation_test.cpp)
target_link_libraries(canvas_voice_isolation_test PRIVATE canvas_core)
set_target_properties(canvas_voice_isolation_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME voice_isolation COMMAND canvas_voice_isolation_test)

add_executable(canvas_vram_leak_test tests/vram_leak_test.cpp)
target_link_libraries(canvas_vram_leak_test PRIVATE canvas_core)
set_target_properties(canvas_vram_leak_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME vram_leak COMMAND canvas_vram_leak_test)

add_executable(canvas_clip_rate_test tests/clip_rate_test.cpp)
target_link_libraries(canvas_clip_rate_test PRIVATE canvas_core)
set_target_properties(canvas_clip_rate_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME clip_rate COMMAND canvas_clip_rate_test)

add_executable(canvas_equalizer_test tests/equalizer_test.cpp)
target_link_libraries(canvas_equalizer_test PRIVATE canvas_core)
set_target_properties(canvas_equalizer_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME equalizer COMMAND canvas_equalizer_test)

# Software-decode sws buffer-sizing + RGBA conversion laws (issue #4 regression
# guard). Synthesized YUV420P frames only — no media file, no device.
add_executable(canvas_sw_decode_test tests/sw_decode_test.cpp)
target_link_libraries(canvas_sw_decode_test PRIVATE canvas_core)
set_target_properties(canvas_sw_decode_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME sw_decode COMMAND canvas_sw_decode_test)

# Real-clip SOFTWARE (CPU-only) decode round trip over the user's genuine AV1
# 2560x1440@60 reel — the software seam of the headless decode path, no
# hardware device anywhere. Test source honors CANVAS_TEST_CLIP (default
# /home/matt/Videos/clips/2026-09-14 09-12-48.mkv); SKIPs (exit 2) when the
# clip is absent, exactly like the vaapi clip tests.
add_executable(canvas_sw_decode_clip_test tests/sw_decode_clip_test.cpp)
target_link_libraries(canvas_sw_decode_clip_test PRIVATE canvas_core)
set_target_properties(canvas_sw_decode_clip_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME sw_decode_clip COMMAND canvas_sw_decode_clip_test)

# Real-clip SOFTWARE (CPU-only, all-FFmpeg-threads) encode round trip: probe
# the genuine 1440p60 reel, export 2560x1440@60 H.265 (libx265) @ 80,000 kbps
# with threads=0 (FFmpeg auto = every core), then decode the output back and
# verify frame count + non-black content. SKIPs (exit 2) when the clip is
# absent or libx265 is unavailable.
add_executable(canvas_sw_encode_clip_test tests/sw_encode_clip_test.cpp)
target_link_libraries(canvas_sw_encode_clip_test PRIVATE canvas_core)
set_target_properties(canvas_sw_encode_clip_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME sw_encode_clip COMMAND canvas_sw_encode_clip_test)

# Software libx265 preset sweep at the same deliver law (informational only:
# validates decode-back per row, never asserts speed). Long timeout: several
# full export_project() runs back to back.
add_executable(canvas_sw_encode_bench tests/sw_encode_bench.cpp)
target_link_libraries(canvas_sw_encode_bench PRIVATE canvas_core)
set_target_properties(canvas_sw_encode_bench PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME sw_encode_bench COMMAND canvas_sw_encode_bench)
set_tests_properties(sw_encode_bench PROPERTIES TIMEOUT 600)

add_executable(canvas_title_test tests/title_test.cpp)
target_link_libraries(canvas_title_test PRIVATE canvas_core)
set_target_properties(canvas_title_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME title COMMAND canvas_title_test)

# The caption-shaping law fed by the transcript seam (AI-subtitle bars). Pure
# math — no model, no devices → always runs.
add_executable(canvas_captions_test tests/captions_test.cpp)
target_link_libraries(canvas_captions_test PRIVATE canvas_core)
set_target_properties(canvas_captions_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME captions COMMAND canvas_captions_test)

add_executable(canvas_transcript_test tests/transcript_test.cpp)
target_link_libraries(canvas_transcript_test PRIVATE canvas_core)
set_target_properties(canvas_transcript_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME transcript COMMAND canvas_transcript_test)

# The whisper.cpp engine seam. The unit portion needs NO model file (error-path
# laws). When a real quantized model is available (CANVAS_WHISPER_MODEL env or
# the default cache path) the same binary also exercises one live 16 kHz
# silence transcription to prove the FetchContent wiring end-to-end; it SKIPs
# that portion alone when no model is present.
add_executable(canvas_transcribe_test tests/transcribe_test.cpp)
target_link_libraries(canvas_transcribe_test PRIVATE canvas_core)
set_target_properties(canvas_transcribe_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME transcribe COMMAND canvas_transcribe_test)

add_executable(canvas_vaapi_encode_test tests/vaapi_encode_test.cpp)
target_link_libraries(canvas_vaapi_encode_test PRIVATE canvas_core)
set_target_properties(canvas_vaapi_encode_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME vaapi_encode COMMAND canvas_vaapi_encode_test)

add_executable(canvas_vaapi_driver_test tests/vaapi_driver_test.cpp)
target_link_libraries(canvas_vaapi_driver_test PRIVATE canvas_core)
set_target_properties(canvas_vaapi_driver_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME vaapi_driver COMMAND canvas_vaapi_driver_test)

add_executable(canvas_gpu_select_test tests/gpu_select_test.cpp)
target_link_libraries(canvas_gpu_select_test PRIVATE canvas_core)
set_target_properties(canvas_gpu_select_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME gpu_select COMMAND canvas_gpu_select_test)
add_executable(canvas_qsv_encode_test tests/qsv_encode_test.cpp)
target_link_libraries(canvas_qsv_encode_test PRIVATE canvas_core)
set_target_properties(canvas_qsv_encode_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME qsv_encode COMMAND canvas_qsv_encode_test)

# Device-bound VAAPI encode regression + speed sweep. Both SKIP (exit 2) when
# the machine has no working VAAPI encode node (no GPU / CI / decode-only
# adapter only). vaapi_enc is a gate (bad output = FAIL), vaapi_enc_bench is
# informational (it validates every output decodes but never asserts speed).
# The bench sweeps dozens of full export_project() runs, so it gets a long
# timeout.
add_executable(canvas_vaapi_enc_test tests/vaapi_enc_test.cpp)
target_link_libraries(canvas_vaapi_enc_test PRIVATE canvas_core)
set_target_properties(canvas_vaapi_enc_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME vaapi_enc COMMAND canvas_vaapi_enc_test)
set_tests_properties(vaapi_enc PROPERTIES TIMEOUT 120)

add_executable(canvas_vaapi_enc_bench tests/vaapi_enc_bench.cpp)
target_link_libraries(canvas_vaapi_enc_bench PRIVATE canvas_core)
set_target_properties(canvas_vaapi_enc_bench PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME vaapi_enc_bench COMMAND canvas_vaapi_enc_bench)
set_tests_properties(vaapi_enc_bench PROPERTIES TIMEOUT 300)

# Device-bound CUDA/NVENC encode regression + speed sweep, the NVIDIA siblings
# of vaapi_enc[_bench]. Same discipline (real encode on the real node, skip=2
# without a CUDA runtime / working NVENC device), and both exercise the full
# title->composite->encode chain via make_feature_project. The bench sweeps
# dozens of full export_project() runs, so it gets a long timeout.
add_executable(canvas_cuda_enc_test tests/cuda_enc_test.cpp)
target_link_libraries(canvas_cuda_enc_test PRIVATE canvas_core)
set_target_properties(canvas_cuda_enc_test PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME cuda_enc COMMAND canvas_cuda_enc_test)
set_tests_properties(cuda_enc PROPERTIES TIMEOUT 120)

add_executable(canvas_cuda_enc_bench tests/cuda_enc_bench.cpp)
target_link_libraries(canvas_cuda_enc_bench PRIVATE canvas_core)
set_target_properties(canvas_cuda_enc_bench PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
add_test(NAME cuda_enc_bench COMMAND canvas_cuda_enc_bench)
set_tests_properties(cuda_enc_bench PROPERTIES TIMEOUT 300)
