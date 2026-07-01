find_package(PkgConfig REQUIRED)

add_library(transport_quic STATIC
    src/quic_transport.cpp
)

target_include_directories(transport_quic
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# libngtcp2 provides QUIC framing and congestion control.
# Use the static library from the system package (libngtcp2-dev).
find_library(NGTCP2_LIB ngtcp2 REQUIRED)
find_path(NGTCP2_INCLUDE ngtcp2/ngtcp2.h REQUIRED)

target_include_directories(transport_quic PRIVATE ${NGTCP2_INCLUDE})
target_link_libraries(transport_quic
    PUBLIC
        pulsar_core
    PRIVATE
        ${NGTCP2_LIB}
)
