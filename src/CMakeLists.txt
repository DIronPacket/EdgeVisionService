add_subdirectory(common)
add_subdirectory(pipeline)
# add_subdirectory(algorithm)
add_subdirectory(comm)      # comm_api：通信基类 + udp/mqtt 子类 + 全局 CommManager
add_subdirectory(upgrade)   # upgrade_api：A/B 槽位 + 清单验签(Ed25519) + 取数 + 健康检查
# add_subdirectory(streamer)
add_subdirectory(platform)
add_subdirectory(main)
