set(TARGET_NAME encoder)
add_library(${TARGET_NAME} ${MODULE_LIB_TYPE})

target_sources(${TARGET_NAME} PRIVATE
    $<$<STREQUAL:${TARGET_PLATFORM},x86_64>:ffmpeg_encoder.cpp>
    $<$<STREQUAL:${TARGET_PLATFORM},rk3588>:rk_mpp_encoder.cpp>
)

target_include_directories(${TARGET_NAME}
PUBLIC
    ${PROJECT_SOURCE_DIR}/src/platform/include
    ${PROJECT_SOURCE_DIR}/src/common/media
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(TARGET_PLATFORM STREQUAL "x86_64")
    find_package(FFMPEG REQUIRED)
    target_include_directories(${TARGET_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(${TARGET_NAME} PRIVATE ${FFMPEG_LIBRARIES})
endif()

if(TARGET_PLATFORM STREQUAL "rk3588")
    find_package(MPP REQUIRED)
    target_include_directories(${TARGET_NAME} PRIVATE ${MPP_INCLUDE_DIRS})
    target_link_libraries(${TARGET_NAME} PRIVATE ${MPP_LIBRARIES})
endif()
