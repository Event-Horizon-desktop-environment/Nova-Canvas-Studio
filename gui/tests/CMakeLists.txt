# Headless unit tests for GUI-side logic that is deliberately Qt-free.
#
# Each test links ONLY the Qt-free sources it exercises (plus canvas_core). No Qt is
# linked here, which is what guarantees the extracted modules stay usable without
# a display — the "unbreakable" seam. The no-Qt invariant is enforced compilation
# time: if a tested module #includes <Q...>, this target simply fails to build.

get_filename_component(CANVAS_GUI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src" ABSOLUTE)

# Build a headless Qt-free test executable from the given test source(s).
function(canvas_add_headless_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CANVAS_GUI_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
    )
    target_link_libraries(${name} PRIVATE canvas_core)
    set_target_properties(${name} PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# sync_constants_test: header-only, no extra object library.
canvas_add_headless_test(sync_constants_test
    sync_constants_test.cpp
)

# timeline_decoder_test: exercises TimelineDecoder headless (Qt-free seam).
# Compiles timeline_decoder.cpp into the test binary so the module is verified
# exactly as shipped (a stray <Q...> include breaks this build on purpose).
canvas_add_headless_test(timeline_decoder_test
    timeline_decoder_test.cpp
    ../src/features/playback/timeline_decoder.cpp
)

# audio_pipeline_test: exercises AudioPipeline headless against an in-memory
# fake sink. Compiles audio_pipeline.cpp directly (same seam guarantee). The
# trim-head + forward-jump cases are the regression guards for the AudioDecoder
# resync-walk fix.
canvas_add_headless_test(audio_pipeline_test
    audio_pipeline_test.cpp
    fake_audio_sink.hpp
    ../src/features/playback/audio_pipeline.cpp
)

# sonicsync_test: exercises SonicSync headless (Qt-free seam). Compiles
# sonicsync.cpp directly (same guarantee: a stray <Q...> include breaks the
# build).
canvas_add_headless_test(sonicsync_test
    sonicsync_test.cpp
    ../src/features/playback/sonicsync.cpp
)

# av_reanchor_test: integration test encoding the MLT "audio rides with its
# frame" invariant — Sonicsync's seek-hold must gate the AudioPipeline feed off
# until the target frame is presented. Compiles both modules directly (Qt-free
# seam enforced by the build).
canvas_add_headless_test(av_reanchor_test
    av_reanchor_test.cpp
    fake_audio_sink.hpp
    ../src/features/playback/sonicsync.cpp
    ../src/features/playback/audio_pipeline.cpp
)

# timeline_snap_test: pure grid-snap math (zoom-dependent quantization). Compiles
# timeline_snap.cpp directly (Qt-free seam enforced by the build).
canvas_add_headless_test(timeline_snap_test
    timeline_snap_test.cpp
    ../src/Widgets/timeline_snap.cpp
)

# timeline_selection_test: selection model (range membership + linked-mate
# coalescing). Compiles timeline_selection.cpp directly (Qt-free seam enforced
# by the build).
canvas_add_headless_test(timeline_selection_test
    timeline_selection_test.cpp
    ../src/Widgets/timeline_selection.cpp
)

# timeline_drag_test: clip-drag session math (grab-frames freeze, snap-aware
# tl_in, clamp-to-zero, same-kind track targeting, release commit decision).
# Compiles timeline_drag.cpp + timeline_snap.cpp directly (Qt-free seam enforced
# by the build).
canvas_add_headless_test(timeline_drag_test
    timeline_drag_test.cpp
    ../src/Widgets/timeline_drag.cpp
    ../src/Widgets/timeline_snap.cpp
)

# transition_handle_editor_test: transition cut-handle session math (duration
# bounds, edge seeding + left/right derivation, drag anchors/clamps, the press/
# commit helpers). Compiles transition_handle_editor.cpp directly (Qt-free seam
# enforced by the build).
canvas_add_headless_test(transition_handle_editor_test
    transition_handle_editor_test.cpp
    ../src/Widgets/transition_handle_editor.cpp
)
