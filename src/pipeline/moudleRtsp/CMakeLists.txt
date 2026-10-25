cmake_minimum_required(VERSION 3.16)
set(TARGET_NAME moudleRtsp)

# 【只写一次 add_library】 SHARED动态库
add_library(${TARGET_NAME} SHARED
    zlm_push_server.cpp
)
find_package(ZLMediaKit REQUIRED)
message(STATUS "==== ZLM_INCLUDE_DIRS = ${ZLM_INCLUDE_DIRS}")
message(STATUS "==== ZLM_LIBRARIES = ${ZLM_LIBRARIES}")
# 对外头文件路径，上层链接 moudleRtsp_api 能看到
target_include_directories(${TARGET_NAME}
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# ZLM 头文件只给模块内部使用，PRIVATE
target_include_directories(${TARGET_NAME}
PUBLIC
    ${ZLM_INCLUDE_DIRS}
)

# 模块本身链接 mk_api
target_link_libraries(${TARGET_NAME} PRIVATE
    -Wl,--start-group
    ${ZLM_LIBRARIES}
    -Wl,--end-group
    pthread
    dl
)
if (UNIX)
    target_link_libraries(${TARGET_NAME} PRIVATE rt)
endif()

