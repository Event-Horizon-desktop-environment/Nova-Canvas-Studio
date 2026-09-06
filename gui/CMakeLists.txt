find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Svg)

include(GNUInstallDirs)
get_filename_component(CANVAS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ui)

qt_standard_project_setup()

qt_add_executable(canvas
    src/main.cpp

    resources.qrc
    src/UX/MainWindow.cpp
    src/UX/MainWindowShell.cpp
    src/UX/ShellMenus.cpp
    src/UX/ShellTopBar.cpp
    src/UX/ShellDocks.cpp
    src/UX/ShellCenter.cpp
    src/Widgets/timeline_snap.cpp
    src/Widgets/timeline_selection.cpp
    src/Widgets/timeline_drag.cpp
    src/Widgets/transition_handle_editor.cpp
    src/UX/theme.cpp
    src/UX/horizon_style.cpp
    ui/MainWindow.ui

    src/features/app/AppActions.cpp
    src/features/project/ProjectActions.cpp
    src/features/timeline/TimelineActions.cpp

    src/features/playback/sequence_controller.cpp
    src/features/playback/audio_pipeline.cpp
    src/features/playback/timeline_decoder.cpp
    src/features/playback/audio_output.cpp
    src/features/playback/sonicsync.cpp
    src/features/playback/playback_controller.cpp
    src/features/thumbnails/thumbnail_service.cpp

    src/features/deliver/deliver_settings_panel.cpp
    src/features/deliver/render_queue_panel.cpp

    src/Widgets/viewer_gl.cpp
    src/Widgets/media_pool_widget.cpp
    src/Widgets/timeline_widget.cpp
    src/Widgets/timeline_view.cpp
    src/Widgets/timeline_interaction.cpp
    src/Widgets/timeline_thumbnails.cpp
    src/Widgets/viewport_selector.cpp
)

target_include_directories(canvas PRIVATE src ui)

target_link_libraries(canvas PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Svg
    canvas_core
)

if(PIPEWIRE_FOUND)
    target_link_libraries(canvas PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(canvas PRIVATE CANVAS_HAVE_PIPEWIRE)
endif()
if(ALSA_FOUND)
    target_link_libraries(canvas PRIVATE PkgConfig::ALSA)
    target_compile_definitions(canvas PRIVATE CANVAS_HAVE_ALSA)
endif()

# ── System install ────────────────────────────────────────────────────────
# `sudo just install` (or `sudo cmake --install build --prefix /usr`) copies
# the binary, the hicolor app icon and the .desktop launcher into /usr.
install(TARGETS canvas
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.svg"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME canvas.svg
)

install(FILES "${CANVAS_SOURCE_ROOT}/packaging/canvas.desktop"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Headless (Qt-free) unit tests for the extracted playback modules.
add_subdirectory(tests)
