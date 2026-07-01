cmake_minimum_required(VERSION 3.20)

add_library(transport_rdp STATIC
    src/rdp_transport.cpp
)

target_include_directories(transport_rdp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(transport_rdp
    PUBLIC
        pulsar_core
)

target_compile_features(transport_rdp PUBLIC cxx_std_20)
