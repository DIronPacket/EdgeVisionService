cmake_minimum_required(VERSION 3.16)
project(EdgeVisionService LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== 默认开启：自研模块编译成动态库.so ==========
option(BUILD_SHARED_MODULE "Build self-written module as shared(.so) or static(.a)" ON)
if(BUILD_SHARED_MODULE)
    set(MODULE_LIB_TYPE SHARED CACHE INTERNAL "library type for our business module")
else()
    set(MODULE_LIB_TYPE STATIC CACHE INTERNAL "library type for our business module")
endif()

# 动态库必需PIC，静态库开启也兼容
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Generate PIC code")

# ====================== 自动识别平台 ======================
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    set(TARGET_PLATFORM "rk3588")
else()
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(TARGET_PLATFORM "x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(TARGET_PLATFORM "rk3588")
    else()
        message(FATAL_ERROR "Unsupported native platform: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()
message(STATUS "Auto detected TARGET_PLATFORM = ${TARGET_PLATFORM}")
message(STATUS "Self module library type: ${MODULE_LIB_TYPE}")

# 全局输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# cmake模块路径
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

add_subdirectory(src)
