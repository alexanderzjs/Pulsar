# encoder/nvenc/linux/CMakeLists.txt
# Platform: Linux — NVIDIA NVENC hardware encoder (CUDA + libnvidia-encode)
cmake_minimum_required(VERSION 3.20)

add_library(encoder_nvenc STATIC
    src/nvenc_encoder.cpp
)

target_include_directories(encoder_nvenc
    PUBLIC
        # Shared public header (one level up from this platform dir).
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        # nvEncodeAPI.h — vendored from nv-codec-headers, private to this impl.
        # To update: apt install libffmpeg-nvenc-dev
        #            cp /usr/include/ffnvcodec/nvEncodeAPI.h linux/include/
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(encoder_nvenc
    PUBLIC
        pulsar_core
    PRIVATE
        # NVENC loads libcuda.so.1 and libnvidia-encode.so.1 at runtime via dlopen.
        ${CMAKE_DL_LIBS}
)

target_compile_features(encoder_nvenc PUBLIC cxx_std_20)
