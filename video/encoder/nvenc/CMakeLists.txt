# encoder/nvenc/CMakeLists.txt
# Platform dispatch: delegates to the platform-specific subdirectory.
# Shared public header lives in include/nvenc_encoder.h.
#
# Supported platforms:
#   linux/    — CUDA context + libnvidia-encode (implemented)
#   windows/  — D3D11 device + nvEncodeAPI.dll  (future)
cmake_minimum_required(VERSION 3.20)

if (UNIX AND NOT APPLE)
    add_subdirectory(linux)
elseif (WIN32)
    # add_subdirectory(windows)  # TODO Step 9 Windows
    message(WARNING "encoder/nvenc: Windows not yet implemented")
endif()
