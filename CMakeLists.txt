cmake_minimum_required(VERSION 3.16)
project(EdgeVisionService LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== 调试信息：崩溃现场必需的编译选项 ==========
# 之前不指定 CMAKE_BUILD_TYPE 时 CMake 用「空 flags」（既无 -g 也无 -O）：
# 二进制虽 not stripped，但没有 DWARF，core dump 里只能看到地址，看不到
# 函数名 / 文件行号 / 局部变量 —— 崩溃定位直接断一半。
# 这里给默认 RelWithDebInfo（-O2 -g -DNDEBUG）：板端性能 + 可调试。
# 需要逐个变量查看时（-O2 会把局部变量放进寄存器、显示 <optimized out>）：
#   make BUILD=out-dbg CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Debug"
# 显式传入的 CMAKE_BUILD_TYPE 不会被覆盖。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Build type: Debug/Release/RelWithDebInfo" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE 未指定 → 默认 RelWithDebInfo（含 -g，便于 core 定位）")
endif()

# 帧指针：backtrace / gdb 靠 x29(FP) 串调用栈，-O2 默认会省略它
add_compile_options(-fno-omit-frame-pointer)

# ========== 默认开启：自研模块编译成动态库.so ==========
option(BUILD_SHARED_MODULE "Build self-written module as shared(.so) or static(.a)" ON)
if(BUILD_SHARED_MODULE)
    set(MODULE_LIB_TYPE SHARED CACHE INTERNAL "library type for our business module")
else()
    set(MODULE_LIB_TYPE STATIC CACHE INTERNAL "library type for our business module")
endif()

# ========== 预处理模块 OSD 是否启用 FreeType 矢量字体 ==========
# 默认 OFF：install/ 里当前没有 freetype，默认构建不引入任何新依赖；
# 关闭时 src/common/preprocess 只用内置点阵字体（.evsfont），freetype 相关代码不参与编译。
# 需要矢量字时：先准备 freetype 头/库，再 cmake -DBUILD_FREETYPE_OSD=ON。
option(BUILD_FREETYPE_OSD "Build OSD text overlay with FreeType (requires freetype headers/libs)" OFF)


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
