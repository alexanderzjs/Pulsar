find_package(PkgConfig REQUIRED)
pkg_check_modules(NICE REQUIRED IMPORTED_TARGET nice)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0)

add_library(transport_webrtc STATIC
    src/webrtc_transport.cpp
)

target_include_directories(transport_webrtc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(transport_webrtc
    PUBLIC
        pulsar_core
        PkgConfig::NICE
        PkgConfig::GLIB
)
