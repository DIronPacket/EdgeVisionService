set(TARGET_NAME pipeline_api)
add_library(${TARGET_NAME} ${MODULE_LIB_TYPE})

target_sources(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_node.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_factory.cpp        # T5：type -> 节点对象
    ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_builder.cpp    # T5：PipelineDesc -> 已连线链路
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_push/moudle_push.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_vi/moudle_vi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_print/moudle_print.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_vpss/moudle_vpss.cpp
)

# 如果是动态库，开启 PIC
set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(${TARGET_NAME}
PUBLIC
    ${PROJECT_SOURCE_DIR}/src/pipeline
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/common/MemManager
    ${PROJECT_SOURCE_DIR}/src/common/profiler
    ${PROJECT_SOURCE_DIR}/src/common/preprocess   # pre::ImagePreprocessor（VpssNode 用）
    ${CMAKE_CURRENT_SOURCE_DIR}/
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_push
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_vi
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_print
    ${CMAKE_CURRENT_SOURCE_DIR}/moudle_vpss
)

# ==========================================
# 【核心修改】链接平台层总库
# ==========================================
target_link_libraries(${TARGET_NAME} 
    PUBLIC 
    common_api    # T5：pipeline_builder 读 NodeConfigManager（符号 + configmgr 头路径）
    platform_api  # <--- 直接链接你之前定义的平台总库
)