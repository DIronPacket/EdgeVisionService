# 1. 编译为动态库 (SHARED)
add_library(common_api SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/MemManager/MemPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MemManager/MemManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/frame_converter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/frame_drawer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/glyph_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/freetype_glyph_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/osd_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/image_preprocessor.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/profiler/
    ${CMAKE_CURRENT_SOURCE_DIR}/preprocess/
)

# 5. 链接线程库
find_package(Threads REQUIRED)
target_link_libraries(common_api PUBLIC Threads::Threads)

# 6. 设置 C++ 标准
target_compile_features(common_api PUBLIC cxx_std_14)

# 7. OSD 矢量字体（可选）：开关 BUILD_FREETYPE_OSD 定义在顶层 CMakeLists.txt，默认 OFF。
#    关闭时 preprocess 只用内置点阵字体（.evsfont），freetype 相关分支不参与编译；
#    开启后 osd_renderer 的 set_truetype_font() 才可能成功。
if(BUILD_FREETYPE_OSD)
    find_package(Freetype REQUIRED)
    target_compile_definitions(common_api PRIVATE EVS_ENABLE_FREETYPE=1)
    target_link_libraries(common_api PUBLIC Freetype::Freetype)
    message(STATUS "preprocess OSD: FreeType enabled (${FREETYPE_INCLUDE_DIRS})")
else()
    message(STATUS "preprocess OSD: FreeType disabled (built-in bitmap font only)")
endif()