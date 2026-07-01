cmake_minimum_required(VERSION 3.20)

add_library(encoder_rdp STATIC
    src/rdp_encoder.cpp
)

target_include_directories(encoder_rdp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(encoder_rdp
    PUBLIC
        pulsar_core
)

target_compile_features(encoder_rdp PUBLIC cxx_std_20)
