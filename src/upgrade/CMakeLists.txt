# ============================================================================
# upgrade_api：升级对象库（A/B 槽位 / 清单 / 验签 / 取数 / 健康检查）
#
# 产物：out/lib/libupgrade_api.so
# 依赖：common_api（json.hpp + crash_handler + 文件工具）、OpenSSL::Crypto
#       （SHA-256 与 Ed25519 都在 libcrypto 里，**不需要 libssl**）
#
# 为什么不做成静态库：与 comm_api / common_api 保持一致，进程之间共享同一份
# 槽位/清单解析实现；.so 换实现时不用重编所有进程。
# ============================================================================
add_library(upgrade_api SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/upgrade_types.cpp   # 11 个 Phase / 原因码的字符串映射
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest.cpp        # manifest.json 解析 + semver + 兼容性判定
    ${CMAKE_CURRENT_SOURCE_DIR}/verifier.cpp        # SHA-256 / Ed25519 验签（§5.4）
    ${CMAKE_CURRENT_SOURCE_DIR}/fetcher.cpp         # file / http(s) 取数（curl 子进程，§5.6）
    ${CMAKE_CURRENT_SOURCE_DIR}/slot_manager.cpp    # releases/current/previous + 原子写（§6.3）
    ${CMAKE_CURRENT_SOURCE_DIR}/health_check.cpp    # RTSP 端口探活（帧率是桩，§7.1）
)

set_target_properties(upgrade_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(upgrade_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_include_directories(upgrade_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/
)

target_link_libraries(upgrade_api PUBLIC common_api)
target_compile_features(upgrade_api PUBLIC cxx_std_14)

# ---------------------------------------------------------------
# OpenSSL：必须用 install/<平台> 那一份（§11.2 的头号构建风险）
#
# 板端现状：install/rk3588/include/openssl = 3.5.4，系统 = 3.0.2。
# 两套 ABI 不完全兼容，混用（用 3.0.2 的头编、运行期加载 3.5.4）会出现
# 「能编过、跑起来才炸」的怪问题。所以这里默认把 OPENSSL_ROOT_DIR 指过去，
# 并且在 configure 阶段把版本 / 路径打出来 —— 出问题第一眼就能看到用的是哪份。
# ---------------------------------------------------------------
set(_evs_ssl_root "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}")
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "${_evs_ssl_root}/include/openssl/opensslv.h")
    set(OPENSSL_ROOT_DIR "${_evs_ssl_root}" CACHE PATH
        "OpenSSL 根目录（默认 install/<平台>，避免与系统自带版本混用）")
endif()

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_link_libraries(upgrade_api PUBLIC OpenSSL::Crypto)

file(STRINGS "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h" _evs_ssl_ver_line
     REGEX "^# *define OPENSSL_VERSION_STR")
string(REGEX REPLACE ".*OPENSSL_VERSION_STR *\"([^\"]+)\".*" "\\1"
       _evs_ssl_ver "${_evs_ssl_ver_line}")
message(STATUS "upgrade_api: OpenSSL ${_evs_ssl_ver} (include=${OPENSSL_INCLUDE_DIR}, crypto=${OPENSSL_CRYPTO_LIBRARY})")
if(NOT OPENSSL_INCLUDE_DIR MATCHES "/install/")
    message(WARNING
        "upgrade_api: OpenSSL 头不是 install/<平台> 下的那一份（系统通常 3.0.2）。"
        "若与运行期加载的 libcrypto 版本不一致会出怪问题，见 DESIGN_UPGRADE §11.2")
endif()

# ---------------------------------------------------------------
# T7 验收：状态码/清单/semver/验签/槽位原子性/取数（源码在 scripts/tests/，
# 由 scripts/tests/run_t7_upgrade.sh 触发）。纯本机 + 临时目录，不碰 /opt。
# ---------------------------------------------------------------
add_executable(test_upgrade
    ${PROJECT_SOURCE_DIR}/scripts/tests/t7_upgrade.cpp
)
target_link_libraries(test_upgrade PRIVATE upgrade_api)
