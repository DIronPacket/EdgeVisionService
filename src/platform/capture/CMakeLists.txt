set(TARGET_NAME capture)
add_library(${TARGET_NAME} ${MODULE_LIB_TYPE})

# 所有平台都编译：ffmpeg采集 + 工厂代码
target_sources(${TARGET_NAME} PRIVATE
    ffmpeg_source/ffmpeg_capture.cpp
    capture_factory.cpp
)

# RK3588平台额外加入V4L2采集实现
if(TARGET_PLATFORM STREQUAL "rk3588")
    target_sources(${TARGET_NAME} PRIVATE
        v4l2_source/v4l2_capture.cpp
    )
endif()

# PUBLIC对外头文件
target_include_directories(${TARGET_NAME}
PUBLIC
    ${PROJECT_SOURCE_DIR}/src/platform/include
    ${PROJECT_SOURCE_DIR}/src/common/media
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_source
)

# RK平台才对外暴露v4l2_source头文件路径
if(TARGET_PLATFORM STREQUAL "rk3588")
    target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/v4l2_source)
    # 导出平台宏，给C++工厂代码使用
    target_compile_definitions(${TARGET_NAME} PUBLIC TARGET_PLATFORM_RK3588)
elseif(TARGET_PLATFORM STREQUAL "x86_64")
    target_compile_definitions(${TARGET_NAME} PUBLIC TARGET_PLATFORM_X86_64)
endif()

# FFmpeg全平台依赖，PRIVATE：内部使用，上层不用链接ffmpeg
find_package(FFMPEG REQUIRED)
target_include_directories(${TARGET_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(${TARGET_NAME} PRIVATE ${FFMPEG_LIBRARIES} pthread)
