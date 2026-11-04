# 1. 编译为动态库 (SHARED)
add_library(common_api SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/MemManager/MemPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MemManager/MemManager.cpp
)

# 2. 动态库必须开启 PIC（位置无关代码）
set_target_properties(common_api PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 3. 统一输出目录，方便其他模块找到 .so
set_target_properties(common_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 4. 暴露头文件路径
target_include_directories(common_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/
    ${CMAKE_CURRENT_SOURCE_DIR}/MemManager/
)

# 5. 链接线程库
find_package(Threads REQUIRED)
target_link_libraries(common_api PUBLIC Threads::Threads)

# 6. 设置 C++ 标准
target_compile_features(common_api PUBLIC cxx_std_14)