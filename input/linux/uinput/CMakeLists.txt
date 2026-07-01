add_library(input_uinput STATIC
    src/uinput_handler.cpp
)

target_include_directories(input_uinput
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(input_uinput
    PUBLIC
        pulsar_core
)
