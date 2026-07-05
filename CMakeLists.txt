cmake_minimum_required(VERSION 3.20)
project(Pulsar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global header-only libs (nlohmann/json, etc.)
include_directories(${CMAKE_SOURCE_DIR}/common/include)

# ── core (platform-agnostic, zero external deps) ─────────────────────────────
add_subdirectory(core)

# ── Linux-only adapter modules ────────────────────────────────────────────────
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(VAAPI    REQUIRED IMPORTED_TARGET libva libva-drm libdrm)

    add_subdirectory(video/capture/linux/drm_virtual)
    add_subdirectory(video/preprocessor/linux/dmabuf)
    add_subdirectory(video/preprocessor/linux/gpu)
    add_subdirectory(video/encoder/vaapi/linux)
    add_subdirectory(video/encoder/nvenc)
    add_subdirectory(transport/rtp/all)
    add_subdirectory(transport/quic/linux)
    add_subdirectory(transport/webrtc/linux)
    add_subdirectory(input/linux/uinput)
    add_subdirectory(audio/capture/linux/pipewire)
    add_subdirectory(audio/encoder/opus/all)
endif()

add_subdirectory(video/preprocessor/all/cpu)
add_subdirectory(video/encoder/x264/all)
add_subdirectory(auth/all/password)

# ── server entry point ────────────────────────────────────────────────────────
add_subdirectory(server)

# ── smoke tests ───────────────────────────────────────────────────────────────
if (UNIX AND NOT APPLE)
    add_executable(test_drm_virtual test/test_drm_virtual.cpp)
    target_link_libraries(test_drm_virtual PRIVATE capture_drm_virtual pulsar_core)
endif()
