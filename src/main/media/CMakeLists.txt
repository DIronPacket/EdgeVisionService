# ============================================================================
# 进程 1：媒体处理服务 → out/bin/evs_media
#
# 逻辑全在 media_service.cpp（与历史入口 EdgeVisionService 共用一份，不复制）。
# 是否参与默认 `cmake --build` 由父目录的 EVS_PROCESS 决定（见 ../CMakeLists.txt）；
# 这里只定义 target，不删 target —— 所以 `cmake --build out --target evs_media`
# 在 EVS_PROCESS=comm 时同样可用。
# ============================================================================
find_package(FFMPEG REQUIRED)
find_package(ZLMediaKit REQUIRED)

add_executable(evs_media
    main_media.cpp
    media_service.cpp
)

target_include_directories(evs_media PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # media_service.h
    ${FFMPEG_INCLUDE_DIRS}
    ${ZLM_INCLUDE_DIRS}
)

target_link_libraries(evs_media
PRIVATE
    common_api      # MemManager / NodeConfigManager / crash_handler
    comm_api        # 媒体进程要上报 media/evt/*（尚未接线，依赖先打通）
    pipeline_api    # PipelineBuilder / 各节点
    platform_api    # 采集与编码后端（头路径也由它 PUBLIC 传播）
    ${FFMPEG_LIBRARIES}
    ${ZLM_LIBRARIES}
)

# 崩溃报告里的调用栈要能显示出可执行文件内的函数名（而不只是 +0x偏移），
# 需要把可执行文件的动态符号表导出：-rdynamic。
target_link_options(evs_media PRIVATE -rdynamic)

# EVS_PROCESS 没选 media 时：target 仍存在，但不进默认 all
if(NOT EVS_WITH_MEDIA)
    set_target_properties(evs_media PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
