find_package(FFMPEG REQUIRED)
add_executable(EdgeVisionService 
    main.cpp
)
target_link_libraries(EdgeVisionService
PRIVATE
    # common
    # pipeline
    # algorithm
    # streamer
    # comm
    platform_api
    ${FFMPEG_LIBRARIES}
)

add_subdirectory(test)
