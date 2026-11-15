# ============================================================================
# 进程 3：升级代理 → out/bin/evs_upgrade
#
# 只链三个库：
#   common_api  → crash_handler.h / json.hpp
#   comm_api    → 命令订阅与事件发布（evs_upgrade 自带通道，不依赖 evs_comm 存活，§9.4）
#   upgrade_api → manifest / verifier / fetcher / slot_manager / health_check
# **不链** pipeline_api / platform_api：升级进程不碰采集编码，故障域最小。
#
# 是否参与默认 `cmake --build` 由父目录的 EVS_PROCESS 决定；
# target 始终存在，所以 `cmake --build out --target evs_upgrade` 随时可用。
# ============================================================================
add_executable(evs_upgrade
    main_upgrade.cpp
    upgrade_service.cpp
)

target_include_directories(evs_upgrade PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(evs_upgrade
PRIVATE
    common_api
    comm_api
    upgrade_api
)

# 崩溃报告里的调用栈要能显示出可执行文件内的函数名：-rdynamic
target_link_options(evs_upgrade PRIVATE -rdynamic)

if(NOT EVS_WITH_UPGRADE)
    set_target_properties(evs_upgrade PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
