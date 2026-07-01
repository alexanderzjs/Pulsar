cmake_minimum_required(VERSION 3.20)

add_library(transport_vnc STATIC
    src/vnc_transport.cpp
)

target_include_directories(transport_vnc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(transport_vnc
    PUBLIC
        pulsar_core
)

target_compile_features(transport_vnc PUBLIC cxx_std_20)
