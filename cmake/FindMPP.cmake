# FindMPP.cmake
# RK MPP，头文件在 rockchip/rk_mpi.h，库名 rockchip_mpp

find_path(MPP_INCLUDE_DIR
    NAMES rockchip/rk_mpi.h
    PATHS
        ${CMAKE_INSTALL_PREFIX}/rk3588/include
        /usr/local/include
        /usr/include
)

find_library(MPP_LIBRARY
    NAMES rockchip_mpp
    PATHS
        ${CMAKE_INSTALL_PREFIX}/rk3588/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/aarch64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MPP
    REQUIRED_VARS MPP_INCLUDE_DIR MPP_LIBRARY
)

if(MPP_FOUND)
    set(MPP_INCLUDE_DIRS ${MPP_INCLUDE_DIR})
    set(MPP_LIBRARIES ${MPP_LIBRARY})
endif()

mark_as_advanced(MPP_INCLUDE_DIR MPP_LIBRARY)
