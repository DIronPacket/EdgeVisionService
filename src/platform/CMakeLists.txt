cmake_minimum_required(VERSION 3.16)
# 平台层总库，对外暴露抽象接口
add_library(platform_api INTERFACE)
target_include_directories(platform_api
INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 子模块开关
option(BUILD_SOURCE_MODULE "Build platform 采集模块" ON)
option(BUILD_DECODER_MODULE "Build platform 解码模块" ON)
option(BUILD_ENCODER_MODULE "Build platform 编码模块" ON)

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
