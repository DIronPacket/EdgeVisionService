find_package(FFMPEG REQUIRED)
find_package(ZLMediaKit REQUIRED)
# add_executable(test_encoder 
#     main_encode.cpp
# )
# target_include_directories(test_encoder PRIVATE ${FFMPEG_INCLUDE_DIRS})
# target_link_libraries(test_encoder
# PRIVATE
#     # common
#     # pipeline
#     # algorithm
#     # streamer
#     # comm
#     platform_api
#     ${FFMPEG_LIBRARIES}
# )

# add_executable(test_pushStream 
#     main_pushStream.cpp
# )
# target_include_directories(test_pushStream PRIVATE ${FFMPEG_INCLUDE_DIRS} ${ZLM_INCLUDE_DIRS})
# target_link_libraries(test_pushStream
# PRIVATE
#     # common
#     # pipeline
#     # algorithm
#     # streamer
#     # comm
#     pipeline_api
#     platform_api
#     ${FFMPEG_LIBRARIES}
#     ${ZLM_LIBRARIES}
# )
add_executable(testnode 
    main_node.cpp
)
target_include_directories(testnode PRIVATE ${FFMPEG_INCLUDE_DIRS} ${ZLM_INCLUDE_DIRS})
target_link_libraries(testnode
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

# ---------------------------------------------------------------
# T5 验收：NodeFactory + PipelineBuilder 单测（源码在 scripts/tests/，
# 由 scripts/tests/run_t5_builder.sh 触发）。
# 只 build 不 start：不碰硬件、不起线程、不开 RTSP。
# ---------------------------------------------------------------
add_executable(test_builder
    ${PROJECT_SOURCE_DIR}/scripts/tests/t5_builder.cpp
)
target_include_directories(test_builder PRIVATE ${FFMPEG_INCLUDE_DIRS} ${ZLM_INCLUDE_DIRS})
target_link_libraries(test_builder
PRIVATE
    common_api
    pipeline_api
    platform_api
    ${FFMPEG_LIBRARIES}
    ${ZLM_LIBRARIES}
)