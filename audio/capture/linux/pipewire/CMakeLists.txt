add_library(audio_capture_pipewire STATIC
    src/pipewire_audio_capture.cpp
)

target_include_directories(audio_capture_pipewire
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(audio_capture_pipewire
    PUBLIC
        pulsar_core
        PkgConfig::PIPEWIRE
)
