cmake_minimum_required(VERSION 3.24)
project(NovaCanvasStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

enable_testing()

add_subdirectory(core)
add_subdirectory(gui)

# Phase 0 (Vulkan-tests): the Vulkan backend is proven test-first. Headless —
# links only canvas_core + Vulkan, never Qt. Disabled when no Vulkan SDK is
# present so ordinary builds (and CI without the headers) stay unaffected.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_subdirectory(Vulkan-tests)
    message(STATUS "Vulkan-tests enabled (Vulkan ${Vulkan_VERSION})")
else()
    message(STATUS "Vulkan headers not found — Vulkan-tests disabled")
endif()

# ── Formatting ──────────────────────────────────────────────────────────────
# Opt-in clang-format enforcement, mirroring MLT's clang-format-check
# discipline. Off by default so ordinary `./build.sh` builds are untouched; the
# agent "before you hand back" loop (and CI, when one exists) configures with
# -DCANVAS_CLANG_FORMAT=ON.
#
#   cmake -DCANVAS_CLANG_FORMAT=ON -B build
#   cmake --build build --target clang-format        # reformat in place
#   cmake --build build --target clang-format-check  # dry-run; fails on drift
#
# The style lives in the repo's .clang-format (Qt-derived: 4-space indent,
# bound-left pointers, 100-column cap).
option(CANVAS_CLANG_FORMAT "Expose clang-format / clang-format-check targets" OFF)
if(CANVAS_CLANG_FORMAT)
    find_program(CLANG_FORMAT clang-format)
    if(NOT CLANG_FORMAT)
        message(WARNING
            "CANVAS_CLANG_FORMAT=ON but clang-format was not found on PATH — "
            "formatting targets disabled.")
    else()
        file(GLOB_RECURSE CANVAS_FORMAT_SOURCES CONFIGURE_DEPENDS
            core/include/*.hpp core/include/*.h core/src/*.c core/src/*.cu
            core/src/*.cpp core/src/*.hpp core/src/*.h core/tests/*.cpp
            gui/src/*.c gui/src/*.cu gui/src/*.cpp gui/src/*.hpp gui/src/*.h
            gui/tests/*.cpp gui/tests/*.hpp gui/tests/*.h
            gui/tests/qt/*.cpp gui/tests/qt/*.hpp gui/tests/qt/*.h
        )
        # clang-format finds the repo .clang-format by walking up from each
        # file, so a relative path list + WORKING_DIRECTORY is all that's needed.
        add_custom_target(clang-format
            COMMAND ${CLANG_FORMAT} -i ${CANVAS_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Reformatting sources with ${CLANG_FORMAT}"
            USES_TERMINAL)
        add_custom_target(clang-format-check
            COMMAND ${CLANG_FORMAT} --dry-run --Werror ${CANVAS_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking formatting: ${CLANG_FORMAT} --dry-run"
            USES_TERMINAL)
    endif()
endif()
