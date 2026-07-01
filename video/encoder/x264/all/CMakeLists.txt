cmake_minimum_required(VERSION 3.20)

add_library(encoder_x264 STATIC
    src/x264_encoder.cpp
)

target_include_directories(encoder_x264
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WIN32)
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/windows_x64/libx264.lib)
elseif (APPLE)
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/macos_universal/libx264.a)
else()
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux_x86_64/libx264.a)
endif()

target_link_libraries(encoder_x264
    PUBLIC  pulsar_core
    PRIVATE ${_X264_LIB} m pthread
)

target_compile_features(encoder_x264 PUBLIC cxx_std_20)
