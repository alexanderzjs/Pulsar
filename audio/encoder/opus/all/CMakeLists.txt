cmake_minimum_required(VERSION 3.20)

add_library(audio_encoder_opus STATIC
    src/opus_encoder.cpp
)

target_include_directories(audio_encoder_opus
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WIN32)
    set(_OPUS_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/windows_x64/libopus.lib)
elseif (APPLE)
    set(_OPUS_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/macos_universal/libopus.a)
else()
    set(_OPUS_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux_x86_64/libopus.a)
endif()

target_link_libraries(audio_encoder_opus
    PUBLIC  pulsar_core
    PRIVATE ${_OPUS_LIB} m
)

target_compile_features(audio_encoder_opus PUBLIC cxx_std_20)
