cmake_minimum_required(VERSION 3.16)

# pipeline对外INTERFACE接口库，上层链接 pipeline_api 即可
add_library(pipeline_api INTERFACE)
target_include_directories(pipeline_api
INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# RTSP模块开关
option(BUILD_RTSP_MODULE "Build pipeline RTSP推拉流模块" ON)

if(BUILD_RTSP_MODULE)
    add_subdirectory(moudleRtsp)
    # 把子模块的api挂到pipeline_api上
    target_link_libraries(pipeline_api INTERFACE moudleRtsp)
endif()
