add_library(auth_password STATIC
    src/password_auth.cpp
)

target_include_directories(auth_password
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(auth_password
    PUBLIC
        pulsar_core
)
