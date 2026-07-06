find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB      REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(XCB_SHM  REQUIRED IMPORTED_TARGET xcb-shm)
pkg_check_modules(XCB_IMG  REQUIRED IMPORTED_TARGET xcb-image)

add_library(capture_xcb STATIC src/xcb_capture.cpp)
target_include_directories(capture_xcb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(capture_xcb
    PUBLIC  pulsar_core
    PRIVATE PkgConfig::XCB PkgConfig::XCB_SHM PkgConfig::XCB_IMG
)
