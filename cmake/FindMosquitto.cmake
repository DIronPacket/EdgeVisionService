# FindMosquitto.cmake
# mosquitto 2.x（MQTT 客户端库），头文件在 mosquitto/mosquitto.h，库名 mosquitto
#
# 为什么走 NO_DEFAULT_PATH：系统 apt 版 mosquitto 与 install/<平台> 里的版本/ABI 不一定一致，
# 混用会在运行期出现「未定义符号 / 版本冲突」。与 FindFFMPEG / FindZLMediaKit 同一策略：
# 只用 install/<平台>，找不到就在 configure 阶段失败（早失败优于运行期段错误）。
#
# 注意：libmosquitto **只有客户端**，broker 是独立可执行（install/<平台>/sbin/mosquitto）。
#       所以本模块只用于 comm_api 里的 MqttComm（客户端角色/服务端角色的订阅端都走它）。

if(NOT DEFINED TARGET_PLATFORM)
    message(FATAL_ERROR "TARGET_PLATFORM not defined!（顶层 CMakeLists.txt 应已自动判定）")
endif()

set(MOSQUITTO_ROOT "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}")

find_path(MOSQUITTO_INCLUDE_DIR
    NAMES mosquitto/mosquitto.h
    HINTS ${MOSQUITTO_ROOT}/include
    NO_DEFAULT_PATH
)

find_library(MOSQUITTO_LIBRARY
    NAMES mosquitto
    HINTS ${MOSQUITTO_ROOT}/lib
    NO_DEFAULT_PATH
)

if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
    message(FATAL_ERROR
        "mosquitto for platform [${TARGET_PLATFORM}] NOT FOUND!\n"
        "  Search path: ${MOSQUITTO_ROOT}\n"
        "  期望：${MOSQUITTO_ROOT}/include/mosquitto/mosquitto.h 与 ${MOSQUITTO_ROOT}/lib/libmosquitto.so\n"
        "  本仓库已装 2.0.22；若换机器，请把 mosquitto 安装到 install/${TARGET_PLATFORM}，"
        "或让该目录指向实际安装位置。")
endif()

set(MOSQUITTO_INCLUDE_DIRS ${MOSQUITTO_INCLUDE_DIR})
set(MOSQUITTO_LIBRARIES ${MOSQUITTO_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Mosquitto
    REQUIRED_VARS MOSQUITTO_INCLUDE_DIRS MOSQUITTO_LIBRARIES
)

mark_as_advanced(MOSQUITTO_INCLUDE_DIR MOSQUITTO_LIBRARY)
