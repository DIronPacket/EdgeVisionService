# FindRGA.cmake
find_path(RGA_INCLUDE_DIR
    NAMES rockchip/rga.h
    PATHS
        ${CMAKE_INSTALL_PREFIX}/rk3588/include
        /usr/local/include
        /usr/include
)

find_library(RGA_LIBRARY
    NAMES rga
    PATHS
        ${CMAKE_INSTALL_PREFIX}/rk3588/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/aarch64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RGA
    REQUIRED_VARS RGA_INCLUDE_DIR RGA_LIBRARY
)

if(RGA_FOUND)
    set(RGA_INCLUDE_DIRS ${RGA_INCLUDE_DIR})
    set(RGA_LIBRARIES ${RGA_LIBRARY})
endif()

mark_as_advanced(RGA_INCLUDE_DIR RGA_LIBRARY)
