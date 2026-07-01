add_library(transport_rtp STATIC
    src/rtp_transport.cpp
)

target_include_directories(transport_rtp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(transport_rtp
    PUBLIC
        pulsar_core
)
