find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(capture_pipewire STATIC
    src/pipewire_capture.cpp
    src/portal_screencast.cpp    # xdg-desktop-portal (GNOME/KDE/wlroots universal)
    src/mutter_screencast.cpp    # GNOME Mutter ScreenCast (private API)
    # src/kwin_screencast.cpp    # KDE KWin ScreenCast (TODO)
)

target_include_directories(capture_pipewire
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(capture_pipewire
    PUBLIC
        pulsar_core
        PkgConfig::PIPEWIRE
    PRIVATE
        PkgConfig::SYSTEMD
)
