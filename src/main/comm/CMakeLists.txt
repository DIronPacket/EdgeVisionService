# ============================================================================
# 进程 2：通信服务 → out/bin/evs_comm
#
# 只链 comm_api（通信对象：CommBase / UdpComm / MqttComm / CommManager）
# 与 common_api（crash_handler + json.hpp）。**不链** pipeline_api / platform_api
# —— 通信进程不碰采集编码，这也是拆进程最主要的收益（故障域与依赖都变小）。
#
# 是否参与默认 `cmake --build` 由父目录的 EVS_PROCESS 决定；
# target 始终存在，所以 `cmake --build out --target evs_comm` 随时可用。
# ============================================================================
add_executable(evs_comm
    main_comm.cpp
)

target_include_directories(evs_comm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(evs_comm
PRIVATE
    common_api      # crash_handler.h / json.hpp（common_api PUBLIC 暴露）
    comm_api        # comm_manager.h / comm_types.h
)

# 崩溃报告里的调用栈要能显示出可执行文件内的函数名：-rdynamic
target_link_options(evs_comm PRIVATE -rdynamic)

if(NOT EVS_WITH_COMM)
    set_target_properties(evs_comm PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
