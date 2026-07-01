cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

add_library(capture_drm_virtual STATIC
    src/drm_virtual_capture.cpp
)

target_include_directories(capture_drm_virtual
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(capture_drm_virtual
    PUBLIC
        pulsar_core
    PRIVATE
        PkgConfig::DRM
)

target_compile_features(capture_drm_virtual PUBLIC cxx_std_20)
