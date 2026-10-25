# rk3588.toolchain.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 改成你自己aarch64-linux-gnu-gcc的实际路径
set(TOOLCHAIN_ROOT "/usr/aarch64-linux-gnu")
set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-g++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
