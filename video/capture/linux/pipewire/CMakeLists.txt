find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(GIO      REQUIRED IMPORTED_TARGET gio-2.0)

add_library(capture_pipewire_video STATIC
    src/pipewire_capture.cpp
)

target_include_directories(capture_pipewire_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(capture_pipewire_video
    PUBLIC  pulsar_core
    PRIVATE PkgConfig::PIPEWIRE PkgConfig::GIO
)
