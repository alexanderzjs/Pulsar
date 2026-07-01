find_package(PkgConfig REQUIRED)
pkg_check_modules(VAAPI_GPU REQUIRED IMPORTED_TARGET libva libva-drm)

add_library(preprocessor_gpu STATIC
    src/gpu_preprocessor.cpp
)

target_include_directories(preprocessor_gpu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(preprocessor_gpu
    PUBLIC
        pulsar_core
        PkgConfig::VAAPI_GPU
)
