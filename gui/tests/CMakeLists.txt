get_filename_component(CANVAS_GUI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src" ABSOLUTE)

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

canvas_add_headless_test(sync_constants_test
    sync_constants_test.cpp
)

canvas_add_headless_test(timeline_decoder_test
    timeline_decoder_test.cpp
    ../src/features/playback/timeline_decoder.cpp
    ../src/features/playback/vaapi_import_state.cpp
)

canvas_add_headless_test(transition_bake_test
    transition_bake_test.cpp
    ../src/features/playback/timeline_decoder.cpp
    ../src/features/playback/vaapi_import_state.cpp
)

canvas_add_headless_test(audio_pipeline_test
    audio_pipeline_test.cpp
    fake_audio_sink.hpp
    ../src/features/playback/audio_pipeline.cpp
)

canvas_add_headless_test(sonicsync_test
    sonicsync_test.cpp
    ../src/features/playback/sonicsync.cpp
)

canvas_add_headless_test(av_reanchor_test
    av_reanchor_test.cpp
    fake_audio_sink.hpp
    ../src/features/playback/sonicsync.cpp
    ../src/features/playback/audio_pipeline.cpp
)

canvas_add_headless_test(timeline_snap_test
    timeline_snap_test.cpp
    ../src/Widgets/timeline_snap.cpp
)

canvas_add_headless_test(timeline_selection_test
    timeline_selection_test.cpp
    ../src/Widgets/timeline_selection.cpp
)

canvas_add_headless_test(timeline_drag_test
    timeline_drag_test.cpp
    ../src/Widgets/timeline_drag.cpp
    ../src/Widgets/timeline_snap.cpp
)

canvas_add_headless_test(transition_handle_editor_test
    transition_handle_editor_test.cpp
    ../src/Widgets/transition_handle_editor.cpp
)

canvas_add_headless_test(audio_targets_test
    audio_targets_test.cpp
    ../src/features/timeline/audio_targets.cpp
)

canvas_add_headless_test(timeline_volume_line_test
    timeline_volume_line_test.cpp
    ../src/Widgets/timeline_volume_line.cpp
)

canvas_add_headless_test(deliver_settings_model_test
    deliver_settings_model_test.cpp
    ../src/features/deliver/deliver_settings_model.cpp
)

canvas_add_headless_test(source_preview_model_test
    source_preview_model_test.cpp
    ../src/features/source_preview/source_preview_model.cpp
)

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

qt_add_executable(thumbs_id_namespace_test
    qt/thumbs_id_namespace_test.cpp
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
    ../src/features/color/mini_timeline_strip.cpp
)
target_include_directories(thumbs_id_namespace_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(thumbs_id_namespace_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Test
    canvas_core
)
set_target_properties(thumbs_id_namespace_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME thumbs_id_namespace_test COMMAND thumbs_id_namespace_test)
set_tests_properties(thumbs_id_namespace_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(thumbs_disk_serve_test
    qt/thumbs_disk_serve_test.cpp
    ../src/features/thumbnails/thumbnail_service.cpp
    ../src/UX/theme_clip_colors.cpp
    ../src/UX/theme_icons.cpp
    ../src/UX/theme_menu.cpp
    ../src/UX/theme_state.cpp
    ../src/UX/theme_styles.cpp
    ../src/UX/theme_tokens.cpp
    ../src/UX/horizon_style.cpp
)
target_include_directories(thumbs_disk_serve_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(thumbs_disk_serve_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    canvas_core
)
set_target_properties(thumbs_disk_serve_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME thumbs_disk_serve_test COMMAND thumbs_disk_serve_test)
set_tests_properties(thumbs_disk_serve_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

qt_add_executable(theme_roundtrip_qt_test
    qt/theme_roundtrip_qt_test.cpp
    ../src/UX/theme_clip_colors.cpp
    ../src/UX/theme_icons.cpp
    ../src/UX/theme_menu.cpp
    ../src/UX/theme_state.cpp
    ../src/UX/theme_styles.cpp
    ../src/UX/theme_tokens.cpp
    ../src/UX/horizon_style.cpp
)
target_include_directories(theme_roundtrip_qt_test PRIVATE
    ${CANVAS_GUI_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(theme_roundtrip_qt_test PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Test
    canvas_core
)
set_target_properties(theme_roundtrip_qt_test PROPERTIES AUTOMOC ON AUTORCC ON AUTOUIC OFF)
add_test(NAME theme_roundtrip_qt_test COMMAND theme_roundtrip_qt_test)
set_tests_properties(theme_roundtrip_qt_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

