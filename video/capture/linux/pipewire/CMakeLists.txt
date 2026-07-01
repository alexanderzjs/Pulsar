add_library(capture_pipewire STATIC
    src/pipewire_capture.cpp
)

target_include_directories(capture_pipewire
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(capture_pipewire
    PUBLIC
        pulsar_core
        PkgConfig::PIPEWIRE
)
