cmake_minimum_required(VERSION 3.20)

add_library(encoder_vnc STATIC
    src/vnc_encoder.cpp
)

target_include_directories(encoder_vnc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(encoder_vnc
    PUBLIC
        pulsar_core
)

target_compile_features(encoder_vnc PUBLIC cxx_std_20)
