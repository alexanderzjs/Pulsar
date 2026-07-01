add_library(preprocessor_cpu STATIC
    src/cpu_preprocessor.cpp
)

target_include_directories(preprocessor_cpu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(preprocessor_cpu
    PUBLIC
        pulsar_core
)
