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
add_executable(testpool 
    main_testpool.cpp
)
target_include_directories(testpool PRIVATE ${FFMPEG_INCLUDE_DIRS} ${ZLM_INCLUDE_DIRS})
target_link_libraries(testpool
PRIVATE
    common_api
    # pipeline
    # algorithm
    # streamer
    # comm
    # test_testpool
    # platform_api
    # ${FFMPEG_LIBRARIES}
    # ${ZLM_LIBRARIES}
)