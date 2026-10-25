# cmake/FindFFMPEG.cmake
# 自动读取顶层自动设置好的 TARGET_PLATFORM
if(NOT DEFINED TARGET_PLATFORM)
    message(FATAL_ERROR "TARGET_PLATFORM not defined!")
endif()

set(FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}")
message(STATUS "Search FFMPEG at: ${FFMPEG_ROOT}")

# 查找头文件，禁止系统路径
find_path(FFMPEG_AVFORMAT_HEADER
    NAMES libavformat/avformat.h
    HINTS ${FFMPEG_ROOT}/include
    NO_DEFAULT_PATH
)

find_library(AVFORMAT_LIB
    NAMES avformat
    HINTS ${FFMPEG_ROOT}/lib
    NO_DEFAULT_PATH
)
find_library(AVCODEC_LIB
    NAMES avcodec
    HINTS ${FFMPEG_ROOT}/lib
    NO_DEFAULT_PATH
)
find_library(AVUTIL_LIB
    NAMES avutil
    HINTS ${FFMPEG_ROOT}/lib
    NO_DEFAULT_PATH
)
find_library(SWSCALE_LIB
    NAMES swscale
    HINTS ${FFMPEG_ROOT}/lib
    NO_DEFAULT_PATH
)
find_library(SWRESAMPLE_LIB
    NAMES swresample
    HINTS ${FFMPEG_ROOT}/lib
    NO_DEFAULT_PATH
)

# 校验
if(NOT FFMPEG_AVFORMAT_HEADER OR NOT AVFORMAT_LIB OR NOT AVCODEC_LIB OR NOT AVUTIL_LIB OR NOT SWSCALE_LIB)
    message(FATAL_ERROR
        "FFMPEG for platform [${TARGET_PLATFORM}] NOT FOUND!\n"
        "Search path: ${FFMPEG_ROOT}\n"
        "Check install/${TARGET_PLATFORM}/include and lib"
    )
endif()

set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
set(FFMPEG_LIBRARIES ${AVFORMAT_LIB} ${SWRESAMPLE_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB} )
set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFMPEG DEFAULT_MSG FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)
mark_as_advanced(FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES FFMPEG_LIB_DIR)
