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
<<<<<<< Updated upstream
=======

# audio_targets_test: mixer-target resolution for multi-clip Volume/Pan edits
# (audio clip direct, linked video->audio mate, no-audio skip, linked-pair
# de-dup, selection-order + duplicate collapse). Compiles audio_targets.cpp
# directly (Qt-free seam enforced by the build).
canvas_add_headless_test(audio_targets_test
    audio_targets_test.cpp
    ../src/features/timeline/audio_targets.cpp
)

# timeline_volume_line_test: audio volume-line math (db<->y law, band clamping,
# monotonicity, degenerate boxes). Compiles timeline_volume_line.cpp directly
# (Qt-free seam enforced by the build).
canvas_add_headless_test(timeline_volume_line_test
    timeline_volume_line_test.cpp
    ../src/Widgets/timeline_volume_line.cpp
)

# deliver_settings_model_test: Deliver-panel policy model (container<->codec
# restriction lists, encoder/preset lists, bitrate-field visibility law).
# Compiles deliver_settings_model.cpp directly (Qt-free seam enforced by the
# build).
canvas_add_headless_test(deliver_settings_model_test
    deliver_settings_model_test.cpp
    ../src/features/deliver/deliver_settings_model.cpp
)

# source_preview_model_test: Dual-Viewer / media-pool-scrub model (synthetic
# single-clip preview project + scrub fraction<->source frame law). Compiles
# source_preview_model.cpp directly (Qt-free seam enforced by the build).
canvas_add_headless_test(source_preview_model_test
    source_preview_model_test.cpp
    ../src/features/source_preview/source_preview_model.cpp
)

# volume_line_drag_qt_test: full-widget (Qt) regression test that drives the
# audio volume-line drag through the real press/move/release handlers and checks
# the emitted preview/commit dB values stay inside the audio law band and point
# the RIGHT way (up = louder, down = quieter). Qt-linked on purpose.
qt_add_executable(volume_line_drag_qt_test
    qt/volume_line_drag_qt_test.cpp
    ../resources.qrc
    ../src/Widgets/timeline_widget.cpp
    ../src/Widgets/timeline_view.cpp
    ../src/Widgets/timeline_interaction.cpp
    ../src/Widgets/timeline_thumbnails.cpp
    ../src/Widgets/timeline_snap.cpp
    ../src/Widgets/timeline_selection.cpp
    ../src/Widgets/timeline_drag.cpp
    ../src/Widgets/transition_handle_editor.cpp
    ../src/Widgets/timeline_volume_line.cpp
    ../src/features/timeline/audio_targets.cpp
    ../src/UX/theme_clip_colors.cpp
    ../src/UX/theme_icons.cpp
    ../src/UX/theme_menu.cpp
    ../src/UX/theme_state.cpp
    ../src/UX/theme_styles.cpp
    ../src/UX/theme_tokens.cpp
    ../src/UX/horizon_style.cpp
    ../src/features/thumbnails/thumbnail_service.cpp
)
target_include_directories(volume_line_drag_qt_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(volume_line_drag_qt_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Test
    canvas_core
)
set_target_properties(volume_line_drag_qt_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME volume_line_drag_qt_test COMMAND volume_line_drag_qt_test)
set_tests_properties(volume_line_drag_qt_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# waveform_placement_qt_test: Qt regression test for the wrong-cut bug. Feeds a
# synthetic waveform through the real on_waveform_ready placement path and pins
# the drawn pixmap to the SAME geometry law frame_at_x() (and the razor) cut on:
# left scene-x == kSceneMargin + kTrackHeaderWidth + tl_in/fpp and pixmap width
# == clip body width. Guards the cx+2 inset AND the width-4 rescale regressions.
qt_add_executable(waveform_placement_qt_test
    qt/waveform_placement_qt_test.cpp
    ../resources.qrc
    ../src/Widgets/timeline_widget.cpp
    ../src/Widgets/timeline_view.cpp
    ../src/Widgets/timeline_interaction.cpp
    ../src/Widgets/timeline_thumbnails.cpp
    ../src/Widgets/timeline_snap.cpp
    ../src/Widgets/timeline_selection.cpp
    ../src/Widgets/timeline_drag.cpp
    ../src/Widgets/transition_handle_editor.cpp
    ../src/Widgets/timeline_volume_line.cpp
    ../src/features/timeline/audio_targets.cpp
    ../src/UX/theme_clip_colors.cpp
    ../src/UX/theme_icons.cpp
    ../src/UX/theme_menu.cpp
    ../src/UX/theme_state.cpp
    ../src/UX/theme_styles.cpp
    ../src/UX/theme_tokens.cpp
    ../src/UX/horizon_style.cpp
    ../src/features/thumbnails/thumbnail_service.cpp
)
target_include_directories(waveform_placement_qt_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(waveform_placement_qt_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Test
    canvas_core
)
set_target_properties(waveform_placement_qt_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME waveform_placement_qt_test COMMAND waveform_placement_qt_test)
set_tests_properties(waveform_placement_qt_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# wheel_panel_roundtrip_qt_test: Qt regression test for the color wheel panel
# (color-grading-phases.md Phase 4). Drives the REAL ColorWheelsPanel handlers
# offscreen (Lift-wheel press/move/release + tone-row spin boxes) and proves the
# emitted WheelPanelState lands in the clip's grade through the same
# one-corrector set_clip_grade seam the Color page uses, survives undo/redo and
# JSON, and round-trips into a fresh panel. Qt-linked on purpose.
qt_add_executable(wheel_panel_roundtrip_qt_test
    qt/wheel_panel_roundtrip_qt_test.cpp
    ../resources.qrc
    ../src/features/color/color_widgets.cpp
    ../src/features/color/scopes/common/scope_common.cpp
    ../src/features/color/scopes/parade/parade_scope.cpp
    ../src/features/color/scopes/waveform/waveform_scope.cpp
    ../src/features/color/scopes/vectorscope/vectorscope_scope.cpp
    ../src/features/color/scopes/histogram/histogram_scope.cpp
    ../src/features/color/scopes/chromaticity/chromaticity_widget.cpp
    ../src/UX/theme_clip_colors.cpp
    ../src/UX/theme_icons.cpp
    ../src/UX/theme_menu.cpp
    ../src/UX/theme_state.cpp
    ../src/UX/theme_styles.cpp
    ../src/UX/theme_tokens.cpp
    ../src/UX/horizon_style.cpp
)
target_include_directories(wheel_panel_roundtrip_qt_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(wheel_panel_roundtrip_qt_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Test
    canvas_core
)
set_target_properties(wheel_panel_roundtrip_qt_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME wheel_panel_roundtrip_qt_test COMMAND wheel_panel_roundtrip_qt_test)
set_tests_properties(wheel_panel_roundtrip_qt_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
>>>>>>> Stashed changes
