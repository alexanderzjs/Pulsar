add_library(pulsar_core STATIC
    src/pipeline.cpp
    src/profiler.cpp
)

target_include_directories(pulsar_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
