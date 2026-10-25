# src/common/CMakeLists.txt
add_library(common_api INTERFACE) # 或者 STATIC (如果有cpp)
target_include_directories(common_api INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/media/
    ${CMAKE_CURRENT_SOURCE_DIR}/
)