# comm_api：通信对象库（udp / mqtt 两种后端 + 全局 CommManager）
#
# 产物：out/lib/libcomm_api.so
# 依赖：Threads（标准）、mosquitto（install/<平台>/lib/libmosquitto.so，已随仓库安装 2.0.22）
#
# 为什么不用 mosquittopp：C++ 包装类会强制继承 + 内部线程模型不透明；
# 直接用 C API 更好控制 Stop 顺序（disconnect → loop 退出 → destroy），代价是写 pimpl。
add_library(comm_api SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/comm_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/comm_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/udp_comm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_comm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/comm_manager.cpp
)

set_target_properties(comm_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(comm_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 头文件对使用方公开：上层只需 include "comm_manager.h" / "comm_types.h"
target_include_directories(comm_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/
)

find_package(Threads REQUIRED)
target_link_libraries(comm_api PUBLIC Threads::Threads)

find_package(Mosquitto REQUIRED)
target_include_directories(comm_api PRIVATE ${MOSQUITTO_INCLUDE_DIRS})
target_link_libraries(comm_api PUBLIC ${MOSQUITTO_LIBRARIES})
message(STATUS "comm_api: mosquitto ${MOSQUITTO_LIBRARIES} (${MOSQUITTO_INCLUDE_DIRS})")

target_compile_features(comm_api PUBLIC cxx_std_14)

# ---------------------------------------------------------------
# T6 验收：通信基类 / UDP / MQTT / CommManager（源码在 scripts/tests/，
# 由 scripts/tests/run_t6_comm.sh 触发）。
# 只跑本机回环：UDP 127.0.0.1 自收自发；MQTT 若 127.0.0.1:1883 没有 broker 则 SKIP。
# ---------------------------------------------------------------
add_executable(test_comm
    ${PROJECT_SOURCE_DIR}/scripts/tests/t6_comm.cpp
)
target_link_libraries(test_comm PRIVATE comm_api)
