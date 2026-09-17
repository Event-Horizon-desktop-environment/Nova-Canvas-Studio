cmake_minimum_required(VERSION 3.24)
project(NovaCanvasStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

enable_testing()

add_subdirectory(core)
add_subdirectory(gui)

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_subdirectory(Vulkan-tests)
    message(STATUS "Vulkan-tests enabled (Vulkan ${Vulkan_VERSION})")
else()
    message(STATUS "Vulkan headers not found — Vulkan-tests disabled")
endif()

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
