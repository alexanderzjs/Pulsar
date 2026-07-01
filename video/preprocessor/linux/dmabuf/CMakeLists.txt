add_library(preprocessor_dmabuf STATIC
    src/dmabuf_importer.cpp
)

target_include_directories(preprocessor_dmabuf
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(preprocessor_dmabuf
    PUBLIC
        pulsar_core
)
