find_package(PkgConfig REQUIRED)
# VAAPI is a system library bound to the GPU driver version — use pkg-config.
pkg_check_modules(VAAPI REQUIRED IMPORTED_TARGET libva libva-drm libdrm)

add_library(encoder_vaapi STATIC
    src/vaapi_encoder.cpp
)

target_include_directories(encoder_vaapi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(encoder_vaapi
    PUBLIC
        pulsar_core
        PkgConfig::VAAPI
)
