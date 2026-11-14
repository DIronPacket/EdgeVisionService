find_package(FFMPEG REQUIRED)
find_package(ZLMediaKit REQUIRED)
add_executable(EdgeVisionService 
    main.cpp
)
target_include_directories(EdgeVisionService PRIVATE ${FFMPEG_INCLUDE_DIRS} ${ZLM_INCLUDE_DIRS})
target_link_libraries(EdgeVisionService
PRIVATE
    common_api
    pipeline_api
    # algorithm
    # streamer
    # comm
    # test_testpool
    platform_api
    ${FFMPEG_LIBRARIES}
    ${ZLM_LIBRARIES}
)

# 崩溃报告里的调用栈要能显示出可执行文件内的函数名（而不只是 +0x偏移），
# 需要把可执行文件的动态符号表导出：-rdynamic。
target_link_options(EdgeVisionService PRIVATE -rdynamic)

# add_subdirectory(test)
