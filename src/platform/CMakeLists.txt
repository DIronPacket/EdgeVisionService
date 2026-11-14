cmake_minimum_required(VERSION 3.16)
# 平台层总库，对外暴露抽象接口
add_library(platform_api INTERFACE)
target_include_directories(platform_api
INTERFACE
    ${PROJECT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 子模块开关
option(BUILD_SOURCE_MODULE "Build platform 采集模块" ON)
option(BUILD_DECODER_MODULE "Build platform 解码模块" ON)
option(BUILD_ENCODER_MODULE "Build platform 编码模块" ON)
# 推流模块：必须声明，否则新建构建目录时该开关默认假 → rtsp target 不加入 →
# pipeline 的 moudle_push / moudle_vi 拿不到 ZLM/FFmpeg 头路径而编译失败。
# 历史坑：旧 build/ 的 CMakeCache.txt 里手工设过 BUILD_RTSP_MODULE=ON，
# 把这个问题掩盖了很久；换构建目录（如 out/）必现。详见 docs/history/TESTLOG.md §5.18。
option(BUILD_RTSP_MODULE "Build platform 推流模块（ZLMediaKit）" ON)

# ========== 采集模块：直接 add_subdirectory(capture)，不再套source层 ==========
if(BUILD_SOURCE_MODULE)
    add_subdirectory(capture)
    target_link_libraries(platform_api INTERFACE capture)
endif()

# 图像预处理模块
if(BUILD_DECODER_MODULE)
    add_subdirectory(decoder)
    target_link_libraries(platform_api INTERFACE decoder)
endif()

# 编解码模块
if(BUILD_ENCODER_MODULE)
    add_subdirectory(encoder)
    target_link_libraries(platform_api INTERFACE encoder)
endif()

# 推流模块
if(BUILD_RTSP_MODULE)
    add_subdirectory(rtsp)
    target_link_libraries(platform_api INTERFACE rtsp)
endif()
