# ============================================================================
# src/main/ —— 进程入口
#
# 目录约定：
#   src/main/media/   进程 1 媒体处理服务 → out/bin/evs_media
#   src/main/comm/    进程 2 通信服务     → out/bin/evs_comm
#   src/main/main.cpp 历史兼容壳          → out/bin/EdgeVisionService（= evs_media 的旧名字）
#   src/main/test/    历史调试入口（默认不构建，见文件末尾注释）
#
# 构建哪些进程：由 EVS_PROCESS 决定，**不指定 = 都构建**
#   cmake -S . -B out                        # 默认 all：evs_media + evs_comm + EdgeVisionService
#   cmake -S . -B out -DEVS_PROCESS=media    # 只编媒体进程
#   cmake -S . -B out -DEVS_PROCESS=comm     # 只编通信进程
#   cmake -S . -B out -DEVS_PROCESS=media,comm
#   make proc PROC=media                     # 同上（顶层 Makefile 包装）
#
# 语义：未选中的 target **仍然定义**，只是 EXCLUDE_FROM_ALL —— 不会在默认
#       `cmake --build out` 里被编译，但 `cmake --build out --target evs_comm`
#       （以及 `make comm`）随时可用。这样临时只编一个进程不需要重新 configure。
# ============================================================================

# ---------- 1. 进程选择（EVS_PROCESS） ----------
set(EVS_PROCESS "all" CACHE STRING
    "要参与默认构建的进程：all（默认）| media | comm | media,comm")

string(TOLOWER "${EVS_PROCESS}" _evs_process_lower)
string(REPLACE "," ";" _evs_process_list "${_evs_process_lower}")

set(EVS_WITH_MEDIA FALSE)
set(EVS_WITH_COMM FALSE)

if(_evs_process_lower STREQUAL "all")
    set(EVS_WITH_MEDIA TRUE)
    set(EVS_WITH_COMM TRUE)
else()
    foreach(_evs_proc IN LISTS _evs_process_list)
        string(STRIP "${_evs_proc}" _evs_proc)
        if(_evs_proc STREQUAL "")
            # 允许 "media," 这种带尾逗号的写法
        elseif(_evs_proc STREQUAL "media")
            set(EVS_WITH_MEDIA TRUE)
        elseif(_evs_proc STREQUAL "comm")
            set(EVS_WITH_COMM TRUE)
        elseif(_evs_proc STREQUAL "upgrade")
            message(FATAL_ERROR
                "EVS_PROCESS=upgrade 尚未实现：src/upgrade/ 与升级进程还没有落地"
                "（见 docs/design/DESIGN_PROCESS_SPLIT.md §7 第 4 步）")
        else()
            message(FATAL_ERROR
                "EVS_PROCESS 不认识 '${_evs_proc}'；可选值：all | media | comm | media,comm")
        endif()
    endforeach()
endif()

if(NOT EVS_WITH_MEDIA AND NOT EVS_WITH_COMM)
    message(FATAL_ERROR "EVS_PROCESS='${EVS_PROCESS}' 没有选中任何进程")
endif()

message(STATUS
    "主进程选择: EVS_PROCESS=${EVS_PROCESS} -> media=${EVS_WITH_MEDIA} comm=${EVS_WITH_COMM}")

# ---------- 2. 历史兼容壳：EdgeVisionService ----------
# 逻辑与 evs_media 完全相同（都调 evs::media::Run），保留旧可执行名是为了
# `make run` 与既有 TESTLOG 记录继续有效。跟随 media 一起编/不编。
find_package(FFMPEG REQUIRED)
find_package(ZLMediaKit REQUIRED)

add_executable(EdgeVisionService
    main.cpp
    media/media_service.cpp
)
target_include_directories(EdgeVisionService PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/media       # media_service.h
    ${FFMPEG_INCLUDE_DIRS}
    ${ZLM_INCLUDE_DIRS}
)
target_link_libraries(EdgeVisionService
PRIVATE
    common_api
    comm_api
    pipeline_api
    platform_api
    ${FFMPEG_LIBRARIES}
    ${ZLM_LIBRARIES}
)

# 崩溃报告里的调用栈要能显示出可执行文件内的函数名（而不只是 +0x偏移），
# 需要把可执行文件的动态符号表导出：-rdynamic。
target_link_options(EdgeVisionService PRIVATE -rdynamic)

if(NOT EVS_WITH_MEDIA)
    set_target_properties(EdgeVisionService PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# ---------- 3. 两个进程 ----------
add_subdirectory(media)
add_subdirectory(comm)

# add_subdirectory(test)   # 历史调试入口（testnode / test_builder），当前有意关闭
